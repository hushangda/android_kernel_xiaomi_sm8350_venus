// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/net.h>
#include <linux/spinlock.h>

#include "thermal_mitigation_device_service_v01.h"

#define QMI_CDEV_DRIVER		"qmi-cooling-device"
#define QMI_TMD_RESP_TOUT	msecs_to_jiffies(100)
#define QMI_CLIENT_NAME_LENGTH	40

struct qmi_cooling_device {
	struct device_node		*np;
	char				cdev_name[THERMAL_NAME_LENGTH];
	char				qmi_name[QMI_CLIENT_NAME_LENGTH];
	bool                            connection_active;
	struct list_head		qmi_node;
	struct thermal_cooling_device	*cdev;
	unsigned int			mtgn_state;
	unsigned int			applied_state;
	bool				state_dirty;
	unsigned int			max_level;
	struct qmi_tmd_instance		*tmd;
};

struct qmi_tmd_instance {
	struct device			*dev;
	struct qmi_handle		handle;
	struct mutex			mutex;
	/* Receive callbacks must not wait for the transaction mutex. */
	spinlock_t			state_lock;
	struct sockaddr_qrtr		server_addr;
	u32				service_generation;
	bool				service_online;
	bool				shutting_down;
	bool				handle_initialized;
	uint32_t			inst_id;
	struct list_head		tmd_cdev_list;
	struct work_struct		svc_arrive_work;
};

static struct qmi_tmd_instance *tmd_instances;
static int tmd_inst_cnt;

static char  device_clients[][QMI_CLIENT_NAME_LENGTH] = {
	{"pa"},
	{"pa_fr1"},
	{"cx_vdd_limit"},
	{"modem"},
	{"modem_current"},
	{"modem_skin"},
	{"modem_bw"},
	{"modem_bw_backoff"},
	{"vbatt_low"},
	{"charge_state"},
	{"mmw0"},
	{"mmw1"},
	{"mmw2"},
	{"mmw3"},
	{"mmw_skin0"},
	{"mmw_skin1"},
	{"mmw_skin2"},
	{"mmw_skin3"},
	{"wlan"},
	{"wlan_bw"},
	{"mmw_skin0_dsc"},
	{"mmw_skin1_dsc"},
	{"mmw_skin2_dsc"},
	{"mmw_skin3_dsc"},
	{"modem_skin_lte_dsc"},
	{"modem_skin_nr_dsc"},
	{"pa_dsc"},
	{"pa_fr1_dsc"},
	{"cdsp_sw"},
	{"cdsp_hw"},
	{"cpuv_restriction_cold"},
	{"cpr_cold"},
	{"modem_lte_dsc"},
	{"modem_nr_dsc"},
	{"modem_nr_scg_dsc"},
	{"sdr0_lte_dsc"},
	{"sdr1_lte_dsc"},
	{"sdr0_nr_dsc"},
	{"sdr1_nr_dsc"},
	{"pa_lte_sdr0_dsc"},
	{"pa_lte_sdr1_dsc"},
	{"pa_nr_sdr0_dsc"},
	{"pa_nr_sdr1_dsc"},
	{"pa_nr_sdr0_scg_dsc"},
	{"pa_nr_sdr1_scg_dsc"},
	{"mmw0_dsc"},
	{"mmw1_dsc"},
	{"mmw2_dsc"},
	{"mmw3_dsc"},
	{"mmw_ific_dsc"},
};

static int qmi_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;

	if (!qmi_cdev)
		return -EINVAL;

	*state = READ_ONCE(qmi_cdev->max_level);

	return 0;
}

static int qmi_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;

	if (!qmi_cdev)
		return -EINVAL;

	*state = READ_ONCE(qmi_cdev->applied_state);

	return 0;
}

static int qmi_tmd_send_state_request(struct qmi_cooling_device *qmi_cdev,
				uint8_t state, struct sockaddr_qrtr *server)
{
	int ret = 0;
	struct tmd_set_mitigation_level_req_msg_v01 req;
	struct tmd_set_mitigation_level_resp_msg_v01 tmd_resp;
	struct qmi_tmd_instance *tmd = qmi_cdev->tmd;
	struct qmi_txn txn;

	memset(&req, 0, sizeof(req));
	memset(&tmd_resp, 0, sizeof(tmd_resp));

	strlcpy(req.mitigation_dev_id.mitigation_dev_id, qmi_cdev->qmi_name,
		QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01);
	req.mitigation_level = state;

	ret = qmi_txn_init(&tmd->handle, &txn,
		tmd_set_mitigation_level_resp_msg_v01_ei, &tmd_resp);
	if (ret < 0) {
		pr_err("qmi set state:%d txn init failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}

	ret = qmi_send_request(&tmd->handle, server, &txn,
			QMI_TMD_SET_MITIGATION_LEVEL_REQ_V01,
			TMD_SET_MITIGATION_LEVEL_REQ_MSG_V01_MAX_MSG_LEN,
			tmd_set_mitigation_level_req_msg_v01_ei, &req);
	if (ret < 0) {
		pr_err("qmi set state:%d txn send failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		qmi_txn_cancel(&txn);
		goto qmi_send_exit;
	}

	ret = qmi_txn_wait(&txn, QMI_TMD_RESP_TOUT);
	if (ret < 0) {
		pr_err("qmi set state:%d txn wait failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}
	if (tmd_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		ret = -EREMOTEIO;
		pr_err("qmi set state:%d NOT success for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}
	ret = 0;
	pr_debug("Requested qmi state:%d for %s\n", state, qmi_cdev->cdev_name);

qmi_send_exit:
	return ret;
}

/* The caller serializes transactions and requested-state changes with mutex. */
static int qmi_tmd_apply_state(struct qmi_cooling_device *qmi_cdev)
{
	struct qmi_tmd_instance *tmd = qmi_cdev->tmd;
	struct sockaddr_qrtr server;
	unsigned int state;
	u32 generation;
	int ret;

	spin_lock(&tmd->state_lock);
	if (tmd->shutting_down) {
		spin_unlock(&tmd->state_lock);
		return -ENODEV;
	}
	if (!tmd->service_online || !qmi_cdev->connection_active) {
		/* Retry a failed device-list query on the next thermal request. */
		if (tmd->service_online)
			queue_work(system_highpri_wq, &tmd->svc_arrive_work);
		spin_unlock(&tmd->state_lock);
		return 0;
	}
	if (!qmi_cdev->state_dirty) {
		spin_unlock(&tmd->state_lock);
		return 0;
	}
	state = qmi_cdev->mtgn_state;
	generation = tmd->service_generation;
	server = tmd->server_addr;
	spin_unlock(&tmd->state_lock);

	ret = qmi_tmd_send_state_request(qmi_cdev, (uint8_t)state, &server);

	spin_lock(&tmd->state_lock);
	if (generation != tmd->service_generation || !tmd->service_online ||
	    !qmi_cdev->connection_active || tmd->shutting_down) {
		/* A reply from the old service cannot confirm the new service. */
		if (!ret)
			ret = -ENETRESET;
	} else if (!ret) {
		qmi_cdev->applied_state = state;
		qmi_cdev->state_dirty = false;
	}
	spin_unlock(&tmd->state_lock);
	return ret;
}

static int qmi_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;
	struct qmi_tmd_instance *tmd;
	int ret;

	if (!qmi_cdev)
		return -EINVAL;

	tmd = qmi_cdev->tmd;
	mutex_lock(&tmd->mutex);
	spin_lock(&tmd->state_lock);
	if (state > qmi_cdev->max_level) {
		spin_unlock(&tmd->state_lock);
		mutex_unlock(&tmd->mutex);
		return -EINVAL;
	}
	if (qmi_cdev->mtgn_state != state ||
	    qmi_cdev->applied_state != state)
		qmi_cdev->state_dirty = true;
	qmi_cdev->mtgn_state = state;
	spin_unlock(&tmd->state_lock);

	/* Failed sends remain dirty, including a repeat of the same target. */
	ret = qmi_tmd_apply_state(qmi_cdev);
	mutex_unlock(&tmd->mutex);
	return ret;
}

static struct thermal_cooling_device_ops qmi_device_ops = {
	.get_max_state = qmi_get_max_state,
	.get_cur_state = qmi_get_cur_state,
	.set_cur_state = qmi_set_cur_state,
};

static int qmi_register_cooling_device(struct qmi_cooling_device *qmi_cdev)
{
	qmi_cdev->cdev = thermal_of_cooling_device_register(
					qmi_cdev->np,
					qmi_cdev->cdev_name,
					qmi_cdev,
					&qmi_device_ops);
	if (IS_ERR(qmi_cdev->cdev)) {
		int ret = PTR_ERR(qmi_cdev->cdev);

		pr_err("Cooling register failed for %s, ret:%ld\n",
			qmi_cdev->cdev_name, PTR_ERR(qmi_cdev->cdev));
		qmi_cdev->cdev = NULL;
		return ret;
	}
	pr_debug("Cooling register success for %s\n", qmi_cdev->cdev_name);

	return 0;
}

static int verify_devices_and_register(struct qmi_tmd_instance *tmd)
{
	struct tmd_get_mitigation_device_list_req_msg_v01 req;
	struct tmd_get_mitigation_device_list_resp_msg_v01 *tmd_resp;
	int ret = 0, i;
	struct qmi_txn txn;
	struct sockaddr_qrtr server;
	u32 generation;

	memset(&req, 0, sizeof(req));
	/* size of tmd_resp is very high, use heap memory rather than stack */
	tmd_resp = kzalloc(sizeof(*tmd_resp), GFP_KERNEL);
	if (!tmd_resp)
		return -ENOMEM;

	mutex_lock(&tmd->mutex);
	spin_lock(&tmd->state_lock);
	if (!tmd->service_online || tmd->shutting_down) {
		spin_unlock(&tmd->state_lock);
		ret = -ENOTCONN;
		goto reg_exit;
	}
	server = tmd->server_addr;
	generation = tmd->service_generation;
	spin_unlock(&tmd->state_lock);
	ret = qmi_txn_init(&tmd->handle, &txn,
		tmd_get_mitigation_device_list_resp_msg_v01_ei, tmd_resp);
	if (ret < 0) {
		pr_err("Transaction Init error for inst_id:0x%x ret:%d\n",
			tmd->inst_id, ret);
		goto reg_exit;
	}

	ret = qmi_send_request(&tmd->handle, &server, &txn,
			QMI_TMD_GET_MITIGATION_DEVICE_LIST_REQ_V01,
			TMD_GET_MITIGATION_DEVICE_LIST_REQ_MSG_V01_MAX_MSG_LEN,
			tmd_get_mitigation_device_list_req_msg_v01_ei,
			&req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		goto reg_exit;
	}

	ret = qmi_txn_wait(&txn, QMI_TMD_RESP_TOUT);
	if (ret < 0) {
		pr_err("Transaction wait error for inst_id:0x%x ret:%d\n",
			tmd->inst_id, ret);
		goto reg_exit;
	}
	if (tmd_resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		ret = -EREMOTEIO;
		pr_err("Get device list NOT success for inst_id:0x%x ret:%d\n",
			tmd->inst_id, ret);
		goto reg_exit;
	}
	mutex_unlock(&tmd->mutex);

	for (i = 0; i < tmd_resp->mitigation_device_list_len; i++) {
		struct qmi_cooling_device *qmi_cdev = NULL;

		list_for_each_entry(qmi_cdev, &tmd->tmd_cdev_list,
					qmi_node) {
			struct tmd_mitigation_dev_list_type_v01 *device =
				&tmd_resp->mitigation_device_list[i];

			if ((strncasecmp(qmi_cdev->qmi_name,
				device->mitigation_dev_id.mitigation_dev_id,
				QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01)))
				continue;

			mutex_lock(&tmd->mutex);
			spin_lock(&tmd->state_lock);
			if (!tmd->service_online || tmd->shutting_down ||
			    generation != tmd->service_generation) {
				spin_unlock(&tmd->state_lock);
				mutex_unlock(&tmd->mutex);
				ret = -ENETRESET;
				goto free_resp;
			}
			qmi_cdev->connection_active = true;
			qmi_cdev->max_level = device->max_mitigation_level;
			qmi_cdev->mtgn_state = min(qmi_cdev->mtgn_state,
						 qmi_cdev->max_level);
			qmi_cdev->state_dirty = true;
			spin_unlock(&tmd->state_lock);
			/*
			 * It is better to set current state
			 * initially or during restart
			 */
			ret = qmi_tmd_apply_state(qmi_cdev);
			mutex_unlock(&tmd->mutex);
			if (!qmi_cdev->cdev)
				ret = qmi_register_cooling_device(qmi_cdev);
			break;
		}
	}

free_resp:
	kfree(tmd_resp);
	return ret;

reg_exit:
	mutex_unlock(&tmd->mutex);
	kfree(tmd_resp);

	return ret;
}

static void qmi_tmd_svc_arrive(struct work_struct *work)
{
	struct qmi_tmd_instance *tmd = container_of(work,
						struct qmi_tmd_instance,
						svc_arrive_work);
	struct qmi_cooling_device *qmi_cdev;

	verify_devices_and_register(tmd);

	/* The thermal path takes cdev->lock before tmd->mutex. */
	list_for_each_entry(qmi_cdev, &tmd->tmd_cdev_list, qmi_node) {
		if (!qmi_cdev->cdev)
			continue;
		mutex_lock(&qmi_cdev->cdev->lock);
		qmi_cdev->cdev->updated = false;
		mutex_unlock(&qmi_cdev->cdev->lock);
	}
}

/* state_lock is held; callbacks only invalidate state, never wait for replies. */
static void qmi_tmd_invalidate(struct qmi_tmd_instance *tmd)
{
	struct qmi_cooling_device *qmi_cdev;

	tmd->service_online = false;
	tmd->service_generation++;
	list_for_each_entry(qmi_cdev, &tmd->tmd_cdev_list, qmi_node) {
		qmi_cdev->connection_active = false;
		qmi_cdev->state_dirty = true;
	}
}

static void thermal_qmi_net_reset(struct qmi_handle *qmi)
{
	struct qmi_tmd_instance *tmd = container_of(qmi,
						struct qmi_tmd_instance,
						handle);

	spin_lock(&tmd->state_lock);
	qmi_tmd_invalidate(tmd);
	spin_unlock(&tmd->state_lock);
}

static void thermal_qmi_del_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd = container_of(qmi,
						struct qmi_tmd_instance,
						handle);

	spin_lock(&tmd->state_lock);
	if (tmd->server_addr.sq_node == service->node &&
	    tmd->server_addr.sq_port == service->port)
		qmi_tmd_invalidate(tmd);
	spin_unlock(&tmd->state_lock);
}

static int thermal_qmi_new_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd = container_of(qmi,
						struct qmi_tmd_instance,
						handle);

	spin_lock(&tmd->state_lock);
	if (!tmd->shutting_down) {
		qmi_tmd_invalidate(tmd);
		tmd->server_addr.sq_family = AF_QIPCRTR;
		tmd->server_addr.sq_node = service->node;
		tmd->server_addr.sq_port = service->port;
		tmd->service_online = true;
		queue_work(system_highpri_wq, &tmd->svc_arrive_work);
	}
	spin_unlock(&tmd->state_lock);
	return 0;
}

static struct qmi_ops thermal_qmi_event_ops = {
	.new_server = thermal_qmi_new_server,
	.del_server = thermal_qmi_del_server,
	.net_reset = thermal_qmi_net_reset,
};

static void qmi_tmd_cleanup(void)
{
	int idx;
	struct qmi_tmd_instance *tmd = tmd_instances;
	struct qmi_cooling_device *qmi_cdev, *c_next;

	for (idx = 0; idx < tmd_inst_cnt; idx++) {
		spin_lock(&tmd[idx].state_lock);
		tmd[idx].shutting_down = true;
		qmi_tmd_invalidate(&tmd[idx]);
		spin_unlock(&tmd[idx].state_lock);
		cancel_work_sync(&tmd[idx].svc_arrive_work);

		/* Drain the sender without blocking QMI's receive callbacks. */
		mutex_lock(&tmd[idx].mutex);
		mutex_unlock(&tmd[idx].mutex);
		if (tmd[idx].handle_initialized) {
			qmi_handle_release(&tmd[idx].handle);
			tmd[idx].handle_initialized = false;
		}
		list_for_each_entry_safe(qmi_cdev, c_next,
				&tmd[idx].tmd_cdev_list, qmi_node) {
			if (!IS_ERR_OR_NULL(qmi_cdev->cdev))
				thermal_cooling_device_unregister(qmi_cdev->cdev);
			list_del(&qmi_cdev->qmi_node);
		}
	}
	tmd_instances = NULL;
	tmd_inst_cnt = 0;
}

static int of_get_qmi_tmd_platform_data(struct device *dev)
{
	int ret = 0, idx = 0, i = 0, subsys_cnt = 0;
	struct device_node *np = dev->of_node;
	struct device_node *subsys_np = NULL, *cdev_np = NULL;
	struct qmi_tmd_instance *tmd;
	struct qmi_cooling_device *qmi_cdev;

	subsys_cnt = of_get_available_child_count(np);
	if (!subsys_cnt) {
		dev_err(dev, "No child node to process\n");
		return -EFAULT;
	}

	tmd = devm_kcalloc(dev, subsys_cnt, sizeof(*tmd), GFP_KERNEL);
	if (!tmd)
		return -ENOMEM;

	for_each_available_child_of_node(np, subsys_np) {
		if (idx >= subsys_cnt) {
			of_node_put(subsys_np);
			break;
		}

		ret = of_property_read_u32(subsys_np, "qcom,instance-id",
				&tmd[idx].inst_id);
		if (ret) {
			dev_err(dev, "error reading qcom,insance-id. ret:%d\n",
				ret);
			goto data_subsys_error;
		}

		tmd[idx].dev = dev;
		mutex_init(&tmd[idx].mutex);
		spin_lock_init(&tmd[idx].state_lock);
		INIT_LIST_HEAD(&tmd[idx].tmd_cdev_list);
		INIT_WORK(&tmd[idx].svc_arrive_work, qmi_tmd_svc_arrive);

		for_each_available_child_of_node(subsys_np, cdev_np) {
			const char *qmi_name;

			qmi_cdev = devm_kzalloc(dev, sizeof(*qmi_cdev),
					GFP_KERNEL);
			if (!qmi_cdev) {
				ret = -ENOMEM;
				goto data_error;
			}

			strlcpy(qmi_cdev->cdev_name, cdev_np->name,
				THERMAL_NAME_LENGTH);

			if (!of_property_read_string(cdev_np,
					"qcom,qmi-dev-name",
					&qmi_name)) {
				strlcpy(qmi_cdev->qmi_name, qmi_name,
						QMI_CLIENT_NAME_LENGTH);
			} else {
				dev_err(dev, "Fail to parse dev name for %s\n",
					cdev_np->name);
				of_node_put(cdev_np);
				break;
			}
			/* Check for supported qmi dev*/
			for (i = 0; i < ARRAY_SIZE(device_clients); i++) {
				if (strcmp(device_clients[i],
					qmi_cdev->qmi_name) == 0)
					break;
			}

			if (i >= ARRAY_SIZE(device_clients)) {
				dev_err(dev, "Not supported dev name for %s\n",
					cdev_np->name);
				of_node_put(cdev_np);
				break;
			}
			qmi_cdev->tmd = &tmd[idx];
			qmi_cdev->np = cdev_np;
			qmi_cdev->mtgn_state = 0;
			qmi_cdev->state_dirty = true;
			list_add(&qmi_cdev->qmi_node, &tmd[idx].tmd_cdev_list);
		}
		idx++;
	}
	of_node_put(np);
	tmd_instances = tmd;
	tmd_inst_cnt = subsys_cnt;

	return 0;
data_error:
	of_node_put(cdev_np);
data_subsys_error:
	of_node_put(subsys_np);
	of_node_put(np);

	return ret;
}

static int qmi_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0, idx = 0;

	ret = of_get_qmi_tmd_platform_data(dev);
	if (ret)
		goto probe_err;

	if (!tmd_instances || !tmd_inst_cnt) {
		dev_err(dev, "Empty tmd instances\n");
		return -EINVAL;
	}

	for (; idx < tmd_inst_cnt; idx++) {
		struct qmi_tmd_instance *tmd = &tmd_instances[idx];

		if (list_empty(&tmd->tmd_cdev_list))
			continue;

		ret = qmi_handle_init(&tmd->handle,
			TMD_GET_MITIGATION_DEVICE_LIST_RESP_MSG_V01_MAX_MSG_LEN,
			&thermal_qmi_event_ops, NULL);
		if (ret < 0) {
			dev_err(dev, "QMI[0x%x] handle init failed. err:%d\n",
					tmd->inst_id, ret);
			goto probe_err;
		}
		tmd->handle_initialized = true;
		ret = qmi_add_lookup(&tmd->handle, TMD_SERVICE_ID_V01,
					TMD_SERVICE_VERS_V01,
					tmd->inst_id);
		if (ret < 0) {
			dev_err(dev, "QMI register failed for 0x%x, ret:%d\n",
				tmd->inst_id, ret);
			goto probe_err;
		}
	}

	return 0;

probe_err:
	qmi_tmd_cleanup();
	return ret;
}

static int qmi_device_remove(struct platform_device *pdev)
{
	qmi_tmd_cleanup();

	return 0;
}

static const struct of_device_id qmi_device_match[] = {
	{.compatible = "qcom,qmi-cooling-devices"},
	{}
};

static struct platform_driver qmi_device_driver = {
	.probe          = qmi_device_probe,
	.remove         = qmi_device_remove,
	.driver         = {
		.name   = QMI_CDEV_DRIVER,
		.of_match_table = qmi_device_match,
	},
};

static int __init qmi_device_init(void)
{
	return platform_driver_register(&qmi_device_driver);
}
module_init(qmi_device_init);

static void __exit qmi_device_exit(void)
{
	platform_driver_unregister(&qmi_device_driver);
}
module_exit(qmi_device_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QTI QMI cooling device driver");
