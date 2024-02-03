// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio I2C Bus Driver
 *
 * The Virtio I2C Specification:
 * https://raw.githubusercontent.com/oasis-tcs/virtio-spec/master/virtio-i2c.tex
 *
 * Copyright (c) 2021 Intel Corporation. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/virtio_i2c.h>
#include <linux/dma-mapping.h>

/**
 * struct virtio_i2c - virtio I2C data
 * @vdev: virtio device for this controller
 * @adap: I2C adapter for this controller
 * @vq: the virtio virtqueue for communication
 */
struct virtio_i2c {
	struct virtio_device *vdev;
	struct i2c_adapter adap;
	struct virtqueue *vq;
};

/**
 * struct virtio_i2c_req - the virtio I2C request structure
 * @completion: completion of virtio I2C message
 * @out_hdr: the OUT header of the virtio I2C message
 * @buf: the buffer into which data is read, or from which it's written
 * @in_hdr: the IN header of the virtio I2C message
 */
struct virtio_i2c_req {
	struct completion *completion;
	dma_addr_t dma_handle;
	struct virtio_i2c_out_hdr out_hdr	____cacheline_aligned;
	uint8_t *buf				____cacheline_aligned;
	struct virtio_i2c_in_hdr in_hdr		____cacheline_aligned;
};

static void virtio_i2c_msg_done(struct virtqueue *vq)
{
	struct virtio_i2c_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vq, &len)))
		complete(req->completion);
}

static void virtio_sg_init(struct scatterlist *sg, void *cpu_addr, unsigned int len)
{
	if (likely(is_vmalloc_addr(cpu_addr))) {
		sg_init_table(sg, 1);
		sg_set_page(sg, vmalloc_to_page(cpu_addr), len,
			    offset_in_page(cpu_addr));
	} else {
		WARN_ON(!virt_addr_valid(cpu_addr));
		sg_init_one(sg, cpu_addr, len);
	}
}

static void get_dma_buffer(struct device *dma_dev, struct virtio_i2c_req *req, struct i2c_msg *msg)
{
	req->buf = dma_alloc_coherent(dma_dev, msg->len, &req->dma_handle, GFP_KERNEL);

	if (req->buf)
		memcpy(req->buf, msg->buf, msg->len);
}

static void put_dma_buffer(struct device *dma_dev, struct virtio_i2c_req *req, struct i2c_msg *msg)
{
	if (msg->flags & I2C_M_RD)
		memcpy(msg->buf, req->buf, msg->len);

	dma_free_coherent(dma_dev, msg->len, req->buf, req->dma_handle);
}

static int virtio_i2c_prepare_reqs(struct virtqueue *vq,
				   struct virtio_i2c_req *reqs,
				   struct i2c_msg *msgs, int num)
{
	struct scatterlist *sgs[3], out_hdr, msg_buf, in_hdr;
	int i;

	for (i = 0; i < num; i++) {
		int outcnt = 0, incnt = 0;

		reqs[i].completion = kcalloc(1, sizeof(struct completion), GFP_KERNEL);
		if (!reqs[i].completion)
			break;

		init_completion(reqs[i].completion);

		/*
		 * Only 7-bit mode supported for this moment. For the address
		 * format, Please check the Virtio I2C Specification.
		 */
		reqs[i].out_hdr.addr = cpu_to_le16(msgs[i].addr << 1);

		if (msgs[i].flags & I2C_M_RD)
			reqs[i].out_hdr.flags |= cpu_to_le32(VIRTIO_I2C_FLAGS_M_RD);

		if (i != num - 1)
			reqs[i].out_hdr.flags |= cpu_to_le32(VIRTIO_I2C_FLAGS_FAIL_NEXT);

		virtio_sg_init(&out_hdr, &reqs[i].out_hdr, sizeof(reqs[i].out_hdr));
		sgs[outcnt++] = &out_hdr;

		if (msgs[i].len) {
			get_dma_buffer(vq->vdev->dev.parent, &reqs[i], &msgs[i]);

			if (!reqs[i].buf) {
				kfree(reqs[i].completion);
				break;
			}

			virtio_sg_init(&msg_buf, reqs[i].buf, msgs[i].len);

			if (msgs[i].flags & I2C_M_RD)
				sgs[outcnt + incnt++] = &msg_buf;
			else
				sgs[outcnt++] = &msg_buf;
		}

		virtio_sg_init(&in_hdr, &reqs[i].in_hdr, sizeof(reqs[i].in_hdr));
		sgs[outcnt + incnt++] = &in_hdr;

		if (virtqueue_add_sgs(vq, sgs, outcnt, incnt, &reqs[i], GFP_KERNEL)) {
			put_dma_buffer(vq->vdev->dev.parent, &reqs[i], &msgs[i]);

			kfree(reqs[i].completion);

			break;
		}
	}

	return i;
}

static int virtio_i2c_complete_reqs(struct virtqueue *vq,
				    struct virtio_i2c_req *reqs,
				    struct i2c_msg *msgs, int num)
{
	bool failed = false;
	int i, j = 0;

	for (i = 0; i < num; i++) {
		struct virtio_i2c_req *req = &reqs[i];

		wait_for_completion(req->completion);

		if (!failed && req->in_hdr.status != VIRTIO_I2C_MSG_OK)
			failed = true;

		put_dma_buffer(vq->vdev->dev.parent, req, &msgs[i]);

		kfree(req->completion);

		if (!failed)
			j++;
	}

	return j;
}

static int virtio_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct virtio_i2c *vi = i2c_get_adapdata(adap);
	struct virtqueue *vq = vi->vq;
	struct virtio_i2c_req *reqs;
	int count;
	dma_addr_t dma_handle;

	reqs = dma_alloc_coherent(vq->vdev->dev.parent,
				  sizeof(struct virtio_i2c_req) * num,
				  &dma_handle, GFP_KERNEL);

	if (!reqs)
		return -ENOMEM;

	count = virtio_i2c_prepare_reqs(vq, reqs, msgs, num);
	if (!count)
		goto err_free;

	/*
	 * For the case where count < num, i.e. we weren't able to queue all the
	 * msgs, ideally we should abort right away and return early, but some
	 * of the messages are already sent to the remote I2C controller and the
	 * virtqueue will be left in undefined state in that case. We kick the
	 * remote here to clear the virtqueue, so we can try another set of
	 * messages later on.
	 */
	virtqueue_kick(vq);

	count = virtio_i2c_complete_reqs(vq, reqs, msgs, count);

err_free:
	dma_free_coherent(vq->vdev->dev.parent,
			  sizeof(struct virtio_i2c_req) * num,
			  reqs, dma_handle);
	return count;
}

static void virtio_i2c_del_vqs(struct virtio_device *vdev)
{
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_i2c_setup_vqs(struct virtio_i2c *vi)
{
	struct virtio_device *vdev = vi->vdev;

	vi->vq = virtio_find_single_vq(vdev, virtio_i2c_msg_done, "msg");
	return PTR_ERR_OR_ZERO(vi->vq);
}

static u32 virtio_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm virtio_algorithm = {
	.master_xfer = virtio_i2c_xfer,
	.functionality = virtio_i2c_func,
};

static int virtio_i2c_probe(struct virtio_device *vdev)
{
	struct virtio_i2c *vi;
	int ret;

	if (!virtio_has_feature(vdev, VIRTIO_I2C_F_ZERO_LENGTH_REQUEST)) {
		dev_err(&vdev->dev, "Zero-length request feature is mandatory\n");
		return -EINVAL;
	}

	vi = devm_kzalloc(&vdev->dev, sizeof(*vi), GFP_KERNEL);
	if (!vi)
		return -ENOMEM;

	vdev->priv = vi;
	vi->vdev = vdev;

	ret = virtio_i2c_setup_vqs(vi);
	if (ret)
		return ret;

	vi->adap.owner = THIS_MODULE;
	snprintf(vi->adap.name, sizeof(vi->adap.name),
		 "i2c_virtio at virtio bus %d", vdev->index);
	vi->adap.algo = &virtio_algorithm;
	vi->adap.dev.parent = &vdev->dev;
	vi->adap.dev.of_node = vdev->dev.of_node;
	i2c_set_adapdata(&vi->adap, vi);

	/*
	 * Setup ACPI node for controlled devices which will be probed through
	 * ACPI.
	 */
	ACPI_COMPANION_SET(&vi->adap.dev, ACPI_COMPANION(vdev->dev.parent));

	ret = i2c_add_adapter(&vi->adap);
	if (ret)
		virtio_i2c_del_vqs(vdev);

	return ret;
}

static void virtio_i2c_remove(struct virtio_device *vdev)
{
	struct virtio_i2c *vi = vdev->priv;

	i2c_del_adapter(&vi->adap);
	virtio_i2c_del_vqs(vdev);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_I2C_ADAPTER, VIRTIO_DEV_ANY_ID },
	{}
};
MODULE_DEVICE_TABLE(virtio, id_table);

static int virtio_i2c_freeze(struct virtio_device *vdev)
{
	virtio_i2c_del_vqs(vdev);
	return 0;
}

static int virtio_i2c_restore(struct virtio_device *vdev)
{
	return virtio_i2c_setup_vqs(vdev->priv);
}

static const unsigned int features[] = {
	VIRTIO_I2C_F_ZERO_LENGTH_REQUEST,
};

static struct virtio_driver virtio_i2c_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.id_table		= id_table,
	.probe			= virtio_i2c_probe,
	.remove			= virtio_i2c_remove,
	.driver			= {
		.name	= "i2c_virtio",
	},
	.freeze			= pm_sleep_ptr(virtio_i2c_freeze),
	.restore		= pm_sleep_ptr(virtio_i2c_restore),
};
module_virtio_driver(virtio_i2c_driver);

MODULE_AUTHOR("Jie Deng <jie.deng@intel.com>");
MODULE_AUTHOR("Conghui Chen <conghui.chen@intel.com>");
MODULE_DESCRIPTION("Virtio i2c bus driver");
MODULE_LICENSE("GPL");
