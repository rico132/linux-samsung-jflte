// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2014, Intel Corporation.
 * Copyright (c) 2017-2021 Christoph Hellwig.
 */
#include <linux/ptrace.h>	/* for force_successful_syscall_return */
#include <linux/nvme_ioctl.h>
#include <linux/io_uring.h>
#include "nvme.h"

/*
 * Convert integer values from ioctl structures to user pointers, silently
 * ignoring the upper bits in the compat case to match behaviour of 32-bit
 * kernels.
 */
static void __user *nvme_to_user_ptr(uintptr_t ptrval)
{
	if (in_compat_syscall())
		ptrval = (compat_uptr_t)ptrval;
	return (void __user *)ptrval;
}

static void *nvme_add_user_metadata(struct bio *bio, void __user *ubuf,
		unsigned len, u32 seed, bool write)
{
	struct bio_integrity_payload *bip;
	int ret = -ENOMEM;
	void *buf;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		goto out;

	ret = -EFAULT;
	if (write && copy_from_user(buf, ubuf, len))
		goto out_free_meta;

	bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
	if (IS_ERR(bip)) {
		ret = PTR_ERR(bip);
		goto out_free_meta;
	}

	bip->bip_iter.bi_size = len;
	bip->bip_iter.bi_sector = seed;
	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
			offset_in_page(buf));
	if (ret == len)
		return buf;
	ret = -ENOMEM;
out_free_meta:
	kfree(buf);
out:
	return ERR_PTR(ret);
}

static int nvme_finish_user_metadata(struct request *req, void __user *ubuf,
		void *meta, unsigned len, int ret)
{
	if (!ret && req_op(req) == REQ_OP_DRV_IN &&
	    copy_to_user(ubuf, meta, len))
		ret = -EFAULT;
	kfree(meta);
	return ret;
}

static struct request *nvme_alloc_user_request(struct request_queue *q,
		struct nvme_command *cmd, void __user *ubuffer,
		unsigned bufflen, void __user *meta_buffer, unsigned meta_len,
		u32 meta_seed, void **metap, unsigned timeout, bool vec,
		unsigned int rq_flags, blk_mq_req_flags_t blk_flags)
{
	bool write = nvme_is_write(cmd);
	struct nvme_ns *ns = q->queuedata;
	struct block_device *bdev = ns ? ns->disk->part0 : NULL;
	struct request *req;
	struct bio *bio = NULL;
	void *meta = NULL;
	int ret;

	req = blk_mq_alloc_request(q, nvme_req_op(cmd) | rq_flags, blk_flags);
	if (IS_ERR(req))
		return req;
	nvme_init_request(req, cmd);

	if (timeout)
		req->timeout = timeout;
	nvme_req(req)->flags |= NVME_REQ_USERCMD;

	if (ubuffer && bufflen) {
		if (!vec)
			ret = blk_rq_map_user(q, req, NULL, ubuffer, bufflen,
				GFP_KERNEL);
		else {
			struct iovec fast_iov[UIO_FASTIOV];
			struct iovec *iov = fast_iov;
			struct iov_iter iter;

			ret = import_iovec(rq_data_dir(req), ubuffer, bufflen,
					UIO_FASTIOV, &iov, &iter);
			if (ret < 0)
				goto out;
			ret = blk_rq_map_user_iov(q, req, NULL, &iter,
					GFP_KERNEL);
			kfree(iov);
		}
		if (ret)
			goto out;
		bio = req->bio;
		if (bdev)
			bio_set_dev(bio, bdev);
		if (bdev && meta_buffer && meta_len) {
			meta = nvme_add_user_metadata(bio, meta_buffer, meta_len,
					meta_seed, write);
			if (IS_ERR(meta)) {
				ret = PTR_ERR(meta);
				goto out_unmap;
			}
			req->cmd_flags |= REQ_INTEGRITY;
			*metap = meta;
		}
	}

	return req;

out_unmap:
	if (bio)
		blk_rq_unmap_user(bio);
out:
	blk_mq_free_request(req);
	return ERR_PTR(ret);
}

static int nvme_submit_user_cmd(struct request_queue *q,
		struct nvme_command *cmd, void __user *ubuffer,
		unsigned bufflen, void __user *meta_buffer, unsigned meta_len,
		u32 meta_seed, u64 *result, unsigned timeout, bool vec)
{
	struct request *req;
	void *meta = NULL;
	struct bio *bio;
	int ret;

	req = nvme_alloc_user_request(q, cmd, ubuffer, bufflen, meta_buffer,
			meta_len, meta_seed, &meta, timeout, vec, 0, 0);
	if (IS_ERR(req))
		return PTR_ERR(req);

	bio = req->bio;

	ret = nvme_execute_passthru_rq(req);

	if (result)
		*result = le64_to_cpu(nvme_req(req)->result.u64);
	if (meta)
		ret = nvme_finish_user_metadata(req, meta_buffer, meta,
						meta_len, ret);
	if (bio)
		blk_rq_unmap_user(bio);
	blk_mq_free_request(req);
	return ret;
}

static int nvme_submit_io(struct nvme_ns *ns, struct nvme_user_io __user *uio)
{
	struct nvme_user_io io;
	struct nvme_command c;
	unsigned length, meta_len;
	void __user *metadata;

	if (copy_from_user(&io, uio, sizeof(io)))
		return -EFAULT;
	if (io.flags)
		return -EINVAL;

	switch (io.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
	case nvme_cmd_compare:
		break;
	default:
		return -EINVAL;
	}

	length = (io.nblocks + 1) << ns->lba_shift;

	if ((io.control & NVME_RW_PRINFO_PRACT) &&
	    ns->ms == sizeof(struct t10_pi_tuple)) {
		/*
		 * Protection information is stripped/inserted by the
		 * controller.
		 */
		if (nvme_to_user_ptr(io.metadata))
			return -EINVAL;
		meta_len = 0;
		metadata = NULL;
	} else {
		meta_len = (io.nblocks + 1) * ns->ms;
		metadata = nvme_to_user_ptr(io.metadata);
	}

	if (ns->features & NVME_NS_EXT_LBAS) {
		length += meta_len;
		meta_len = 0;
	} else if (meta_len) {
		if ((io.metadata & 3) || !io.metadata)
			return -EINVAL;
	}

	memset(&c, 0, sizeof(c));
	c.rw.opcode = io.opcode;
	c.rw.flags = io.flags;
	c.rw.nsid = cpu_to_le32(ns->head->ns_id);
	c.rw.slba = cpu_to_le64(io.slba);
	c.rw.length = cpu_to_le16(io.nblocks);
	c.rw.control = cpu_to_le16(io.control);
	c.rw.dsmgmt = cpu_to_le32(io.dsmgmt);
	c.rw.reftag = cpu_to_le32(io.reftag);
	c.rw.apptag = cpu_to_le16(io.apptag);
	c.rw.appmask = cpu_to_le16(io.appmask);

	return nvme_submit_user_cmd(ns->queue, &c,
			nvme_to_user_ptr(io.addr), length,
			metadata, meta_len, lower_32_bits(io.slba), NULL, 0,
			false);
}

static bool nvme_validate_passthru_nsid(struct nvme_ctrl *ctrl,
					struct nvme_ns *ns, __u32 nsid)
{
	if (ns && nsid != ns->head->ns_id) {
		dev_err(ctrl->device,
			"%s: nsid (%u) in cmd does not match nsid (%u)"
			"of namespace\n",
			current->comm, nsid, ns->head->ns_id);
		return false;
	}

	return true;
}

static int nvme_user_cmd(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			struct nvme_passthru_cmd __user *ucmd)
{
	struct nvme_passthru_cmd cmd;
	struct nvme_command c;
	unsigned timeout = 0;
	u64 result;
	int status;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid))
		return -EINVAL;

	memset(&c, 0, sizeof(c));
	c.common.opcode = cmd.opcode;
	c.common.flags = cmd.flags;
	c.common.nsid = cpu_to_le32(cmd.nsid);
	c.common.cdw2[0] = cpu_to_le32(cmd.cdw2);
	c.common.cdw2[1] = cpu_to_le32(cmd.cdw3);
	c.common.cdw10 = cpu_to_le32(cmd.cdw10);
	c.common.cdw11 = cpu_to_le32(cmd.cdw11);
	c.common.cdw12 = cpu_to_le32(cmd.cdw12);
	c.common.cdw13 = cpu_to_le32(cmd.cdw13);
	c.common.cdw14 = cpu_to_le32(cmd.cdw14);
	c.common.cdw15 = cpu_to_le32(cmd.cdw15);

	if (cmd.timeout_ms)
		timeout = msecs_to_jiffies(cmd.timeout_ms);

	status = nvme_submit_user_cmd(ns ? ns->queue : ctrl->admin_q, &c,
			nvme_to_user_ptr(cmd.addr), cmd.data_len,
			nvme_to_user_ptr(cmd.metadata), cmd.metadata_len,
			0, &result, timeout, false);

	if (status >= 0) {
		if (put_user(result, &ucmd->result))
			return -EFAULT;
	}

	return status;
}

static int nvme_user_cmd64(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			struct nvme_passthru_cmd64 __user *ucmd, bool vec)
{
	struct nvme_passthru_cmd64 cmd;
	struct nvme_command c;
	unsigned timeout = 0;
	int status;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid))
		return -EINVAL;

	memset(&c, 0, sizeof(c));
	c.common.opcode = cmd.opcode;
	c.common.flags = cmd.flags;
	c.common.nsid = cpu_to_le32(cmd.nsid);
	c.common.cdw2[0] = cpu_to_le32(cmd.cdw2);
	c.common.cdw2[1] = cpu_to_le32(cmd.cdw3);
	c.common.cdw10 = cpu_to_le32(cmd.cdw10);
	c.common.cdw11 = cpu_to_le32(cmd.cdw11);
	c.common.cdw12 = cpu_to_le32(cmd.cdw12);
	c.common.cdw13 = cpu_to_le32(cmd.cdw13);
	c.common.cdw14 = cpu_to_le32(cmd.cdw14);
	c.common.cdw15 = cpu_to_le32(cmd.cdw15);

	if (cmd.timeout_ms)
		timeout = msecs_to_jiffies(cmd.timeout_ms);

	status = nvme_submit_user_cmd(ns ? ns->queue : ctrl->admin_q, &c,
			nvme_to_user_ptr(cmd.addr), cmd.data_len,
			nvme_to_user_ptr(cmd.metadata), cmd.metadata_len,
			0, &cmd.result, timeout, vec);

	if (status >= 0) {
		if (put_user(cmd.result, &ucmd->result))
			return -EFAULT;
	}

	return status;
}

struct nvme_uring_data {
	__u64	metadata;
	__u64	addr;
	__u32	data_len;
	__u32	metadata_len;
	__u32	timeout_ms;
};

/*
 * This overlays struct io_uring_cmd pdu.
 * Expect build errors if this grows larger than that.
 */
struct nvme_uring_cmd_pdu {
	union {
		struct bio *bio;
		struct request *req;
	};
	void *meta; /* kernel-resident buffer */
	void __user *meta_buffer;
	u32 meta_len;
};

static inline struct nvme_uring_cmd_pdu *nvme_uring_cmd_pdu(
		struct io_uring_cmd *ioucmd)
{
	return (struct nvme_uring_cmd_pdu *)&ioucmd->pdu;
}

static void nvme_uring_task_cb(struct io_uring_cmd *ioucmd)
{
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);
	struct request *req = pdu->req;
	struct bio *bio = req->bio;
	int status;
	u64 result;

	if (nvme_req(req)->flags & NVME_REQ_CANCELLED)
		status = -EINTR;
	else
		status = nvme_req(req)->status;

	result = le64_to_cpu(nvme_req(req)->result.u64);

	if (pdu->meta)
		status = nvme_finish_user_metadata(req, pdu->meta_buffer,
					pdu->meta, pdu->meta_len, status);
	if (bio)
		blk_rq_unmap_user(bio);
	blk_mq_free_request(req);

	io_uring_cmd_done(ioucmd, status, result);
}

static void nvme_uring_cmd_end_io(struct request *req, blk_status_t err)
{
	struct io_uring_cmd *ioucmd = req->end_io_data;
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);
	/* extract bio before reusing the same field for request */
	struct bio *bio = pdu->bio;

	pdu->req = req;
	req->bio = bio;
	/* this takes care of moving rest of completion-work to task context */
	io_uring_cmd_complete_in_task(ioucmd, nvme_uring_task_cb);
}

static int nvme_uring_cmd_io(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		struct io_uring_cmd *ioucmd, unsigned int issue_flags, bool vec)
{
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);
	const struct nvme_uring_cmd *cmd = ioucmd->cmd;
	struct request_queue *q = ns ? ns->queue : ctrl->admin_q;
	struct nvme_uring_data d;
	struct nvme_command c;
	struct request *req;
	unsigned int rq_flags = 0;
	blk_mq_req_flags_t blk_flags = 0;
	void *meta = NULL;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	c.common.opcode = READ_ONCE(cmd->opcode);
	c.common.flags = READ_ONCE(cmd->flags);
	if (c.common.flags)
		return -EINVAL;

	c.common.command_id = 0;
	c.common.nsid = cpu_to_le32(cmd->nsid);
	if (!nvme_validate_passthru_nsid(ctrl, ns, le32_to_cpu(c.common.nsid)))
		return -EINVAL;

	c.common.cdw2[0] = cpu_to_le32(READ_ONCE(cmd->cdw2));
	c.common.cdw2[1] = cpu_to_le32(READ_ONCE(cmd->cdw3));
	c.common.metadata = 0;
	c.common.dptr.prp1 = c.common.dptr.prp2 = 0;
	c.common.cdw10 = cpu_to_le32(READ_ONCE(cmd->cdw10));
	c.common.cdw11 = cpu_to_le32(READ_ONCE(cmd->cdw11));
	c.common.cdw12 = cpu_to_le32(READ_ONCE(cmd->cdw12));
	c.common.cdw13 = cpu_to_le32(READ_ONCE(cmd->cdw13));
	c.common.cdw14 = cpu_to_le32(READ_ONCE(cmd->cdw14));
	c.common.cdw15 = cpu_to_le32(READ_ONCE(cmd->cdw15));

	d.metadata = READ_ONCE(cmd->metadata);
	d.addr = READ_ONCE(cmd->addr);
	d.data_len = READ_ONCE(cmd->data_len);
	d.metadata_len = READ_ONCE(cmd->metadata_len);
	d.timeout_ms = READ_ONCE(cmd->timeout_ms);

	if (issue_flags & IO_URING_F_NONBLOCK) {
		rq_flags = REQ_NOWAIT;
		blk_flags = BLK_MQ_REQ_NOWAIT;
	}

	req = nvme_alloc_user_request(q, &c, nvme_to_user_ptr(d.addr),
			d.data_len, nvme_to_user_ptr(d.metadata),
			d.metadata_len, 0, &meta, d.timeout_ms ?
			msecs_to_jiffies(d.timeout_ms) : 0, vec, rq_flags,
			blk_flags);
	if (IS_ERR(req))
		return PTR_ERR(req);
	req->end_io_data = ioucmd;

	/* to free bio on completion, as req->bio will be null at that time */
	pdu->bio = req->bio;
	pdu->meta = meta;
	pdu->meta_buffer = nvme_to_user_ptr(d.metadata);
	pdu->meta_len = d.metadata_len;

	blk_execute_rq_nowait(req, 0, nvme_uring_cmd_end_io);
	return -EIOCBQUEUED;
}

static bool is_ctrl_ioctl(unsigned int cmd)
{
	if (cmd == NVME_IOCTL_ADMIN_CMD || cmd == NVME_IOCTL_ADMIN64_CMD)
		return true;
	if (is_sed_ioctl(cmd))
		return true;
	return false;
}

static int nvme_ctrl_ioctl(struct nvme_ctrl *ctrl, unsigned int cmd,
		void __user *argp)
{
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp);
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, false);
	default:
		return sed_ioctl(ctrl->opal_dev, cmd, argp);
	}
}

#ifdef COMPAT_FOR_U64_ALIGNMENT
struct nvme_user_io32 {
	__u8	opcode;
	__u8	flags;
	__u16	control;
	__u16	nblocks;
	__u16	rsvd;
	__u64	metadata;
	__u64	addr;
	__u64	slba;
	__u32	dsmgmt;
	__u32	reftag;
	__u16	apptag;
	__u16	appmask;
} __attribute__((__packed__));
#define NVME_IOCTL_SUBMIT_IO32	_IOW('N', 0x42, struct nvme_user_io32)
#endif /* COMPAT_FOR_U64_ALIGNMENT */

static int nvme_ns_ioctl(struct nvme_ns *ns, unsigned int cmd,
		void __user *argp)
{
	switch (cmd) {
	case NVME_IOCTL_ID:
		force_successful_syscall_return();
		return ns->head->ns_id;
	case NVME_IOCTL_IO_CMD:
		return nvme_user_cmd(ns->ctrl, ns, argp);
	/*
	 * struct nvme_user_io can have different padding on some 32-bit ABIs.
	 * Just accept the compat version as all fields that are used are the
	 * same size and at the same offset.
	 */
#ifdef COMPAT_FOR_U64_ALIGNMENT
	case NVME_IOCTL_SUBMIT_IO32:
#endif
	case NVME_IOCTL_SUBMIT_IO:
		return nvme_submit_io(ns, argp);
	case NVME_IOCTL_IO64_CMD:
		return nvme_user_cmd64(ns->ctrl, ns, argp, false);
	case NVME_IOCTL_IO64_CMD_VEC:
		return nvme_user_cmd64(ns->ctrl, ns, argp, true);
	default:
		return -ENOTTY;
	}
}

static int __nvme_ioctl(struct nvme_ns *ns, unsigned int cmd, void __user *arg)
{
       if (is_ctrl_ioctl(cmd))
               return nvme_ctrl_ioctl(ns->ctrl, cmd, arg);
       return nvme_ns_ioctl(ns, cmd, arg);
}

int nvme_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct nvme_ns *ns = bdev->bd_disk->private_data;

	return __nvme_ioctl(ns, cmd, (void __user *)arg);
}

long nvme_ns_chr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct nvme_ns *ns =
		container_of(file_inode(file)->i_cdev, struct nvme_ns, cdev);

	return __nvme_ioctl(ns, cmd, (void __user *)arg);
}

static int nvme_ns_uring_cmd(struct nvme_ns *ns, struct io_uring_cmd *ioucmd,
			     unsigned int issue_flags)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	int ret;

	BUILD_BUG_ON(sizeof(struct nvme_uring_cmd_pdu) > sizeof(ioucmd->pdu));

	/* IOPOLL not supported yet */
	if (issue_flags & IO_URING_F_IOPOLL)
		return -EOPNOTSUPP;

	/* NVMe passthrough requires bit SQE/CQE support */
	if ((issue_flags & (IO_URING_F_SQE128|IO_URING_F_CQE32)) !=
	    (IO_URING_F_SQE128|IO_URING_F_CQE32))
		return -EOPNOTSUPP;

	switch (ioucmd->cmd_op) {
	case NVME_URING_CMD_IO:
		ret = nvme_uring_cmd_io(ctrl, ns, ioucmd, issue_flags, false);
		break;
	case NVME_URING_CMD_IO_VEC:
		ret = nvme_uring_cmd_io(ctrl, ns, ioucmd, issue_flags, true);
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

int nvme_ns_chr_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct nvme_ns *ns = container_of(file_inode(ioucmd->file)->i_cdev,
			struct nvme_ns, cdev);

	return nvme_ns_uring_cmd(ns, ioucmd, issue_flags);
}

#ifdef CONFIG_NVME_MULTIPATH
static int nvme_ns_head_ctrl_ioctl(struct nvme_ns *ns, unsigned int cmd,
		void __user *argp, struct nvme_ns_head *head, int srcu_idx)
	__releases(&head->srcu)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	int ret;

	nvme_get_ctrl(ns->ctrl);
	srcu_read_unlock(&head->srcu, srcu_idx);
	ret = nvme_ctrl_ioctl(ns->ctrl, cmd, argp);

	nvme_put_ctrl(ctrl);
	return ret;
}

int nvme_ns_head_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct nvme_ns_head *head = bdev->bd_disk->private_data;
	void __user *argp = (void __user *)arg;
	struct nvme_ns *ns;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (!ns)
		goto out_unlock;

	/*
	 * Handle ioctls that apply to the controller instead of the namespace
	 * seperately and drop the ns SRCU reference early.  This avoids a
	 * deadlock when deleting namespaces using the passthrough interface.
	 */
	if (is_ctrl_ioctl(cmd))
		return nvme_ns_head_ctrl_ioctl(ns, cmd, argp, head, srcu_idx);

	ret = nvme_ns_ioctl(ns, cmd, argp);
out_unlock:
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

long nvme_ns_head_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct nvme_ns_head *head =
		container_of(cdev, struct nvme_ns_head, cdev);
	void __user *argp = (void __user *)arg;
	struct nvme_ns *ns;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (!ns)
		goto out_unlock;

	if (is_ctrl_ioctl(cmd))
		return nvme_ns_head_ctrl_ioctl(ns, cmd, argp, head, srcu_idx);

	ret = nvme_ns_ioctl(ns, cmd, argp);
out_unlock:
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

int nvme_ns_head_chr_uring_cmd(struct io_uring_cmd *ioucmd,
		unsigned int issue_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct nvme_ns_head *head = container_of(cdev, struct nvme_ns_head, cdev);
	int srcu_idx = srcu_read_lock(&head->srcu);
	struct nvme_ns *ns = nvme_find_path(head);
	int ret = -EINVAL;

	if (ns)
		ret = nvme_ns_uring_cmd(ns, ioucmd, issue_flags);
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}
#endif /* CONFIG_NVME_MULTIPATH */

static int nvme_dev_user_cmd(struct nvme_ctrl *ctrl, void __user *argp)
{
	struct nvme_ns *ns;
	int ret;

	down_read(&ctrl->namespaces_rwsem);
	if (list_empty(&ctrl->namespaces)) {
		ret = -ENOTTY;
		goto out_unlock;
	}

	ns = list_first_entry(&ctrl->namespaces, struct nvme_ns, list);
	if (ns != list_last_entry(&ctrl->namespaces, struct nvme_ns, list)) {
		dev_warn(ctrl->device,
			"NVME_IOCTL_IO_CMD not supported when multiple namespaces present!\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	dev_warn(ctrl->device,
		"using deprecated NVME_IOCTL_IO_CMD ioctl on the char device!\n");
	kref_get(&ns->kref);
	up_read(&ctrl->namespaces_rwsem);

	ret = nvme_user_cmd(ctrl, ns, argp);
	nvme_put_ns(ns);
	return ret;

out_unlock:
	up_read(&ctrl->namespaces_rwsem);
	return ret;
}

long nvme_dev_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct nvme_ctrl *ctrl = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp);
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, false);
	case NVME_IOCTL_IO_CMD:
		return nvme_dev_user_cmd(ctrl, argp);
	case NVME_IOCTL_RESET:
		dev_warn(ctrl->device, "resetting controller\n");
		return nvme_reset_ctrl_sync(ctrl);
	case NVME_IOCTL_SUBSYS_RESET:
		return nvme_reset_subsystem(ctrl);
	case NVME_IOCTL_RESCAN:
		nvme_queue_scan(ctrl);
		return 0;
	default:
		return -ENOTTY;
	}
}
