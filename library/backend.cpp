#include <memory>
#include <fcntl.h>
#include "elliptics.h"
#include "../monitor/monitor.hpp"
#include <fstream>

static int dnet_ids_generate(struct dnet_node *n, const char *file, unsigned long long storage_free)
{
	const unsigned long long size_per_id = 100 * 1024 * 1024 * 1024ULL;
	const size_t num = storage_free / size_per_id + 1;
	dnet_raw_id tmp;
	const char *random_source = "/dev/urandom";

	std::ifstream in(random_source, std::ofstream::binary);
	if (!in) {
		int err = -errno;
		dnet_log_err(n, "failed to open '%s' as source of ids file '%s'", random_source, file);
		return err;
	}

	std::ofstream out(file, std::ofstream::binary | std::ofstream::trunc);
	if (!out) {
		int err = -errno;
		dnet_log_err(n, "failed to open/create ids file '%s'", file);
		return err;
	}

	for (size_t i = 0; i < num; ++i) {
		if (!in.read(reinterpret_cast<char *>(tmp.id), sizeof(tmp.id))) {
			int err = -errno;
			dnet_log_err(n, "failed to read id from '%s'", random_source);
			return err;
		}

		if (!out.write(reinterpret_cast<char *>(tmp.id), sizeof(tmp.id))) {
			int err = -errno;
			dnet_log_err(n, "failed to write id into ids file '%s'", file);
			return err;
		}
	}

	return 0;
}

static struct dnet_raw_id *dnet_ids_init(struct dnet_node *n, const char *hdir, int *id_num, unsigned long long storage_free, struct dnet_addr *cfg_addrs, size_t backend_id)
{
	int fd, err, num;
	const char *file = "ids";
	char path[strlen(hdir) + 1 + strlen(file) + 1]; /* / + null-byte */
	struct stat st;
	struct dnet_raw_id *ids;

	snprintf(path, sizeof(path), "%s/%s", hdir, file);

again:
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		if (err == -ENOENT) {
			if (n->flags & DNET_CFG_KEEPS_IDS_IN_CLUSTER)
				err = dnet_ids_update(n, 1, path, cfg_addrs, backend_id);
			if (err)
				err = dnet_ids_generate(n, path, storage_free);

			if (err)
				goto err_out_exit;

			goto again;
		}

		dnet_log_err(n, "failed to open ids file '%s'", path);
		goto err_out_exit;
	}

	err = fstat(fd, &st);
	if (err)
		goto err_out_close;

	if (st.st_size % sizeof(struct dnet_raw_id)) {
		dnet_log(n, DNET_LOG_ERROR, "Ids file size (%lu) is wrong, must be modulo of raw ID size (%zu).\n",
				(unsigned long)st.st_size, sizeof(struct dnet_raw_id));
		goto err_out_close;
	}

	num = st.st_size / sizeof(struct dnet_raw_id);
	if (!num) {
		dnet_log(n, DNET_LOG_ERROR, "No ids read, exiting.\n");
		err = -EINVAL;
		goto err_out_close;
	}

	if (n->flags & DNET_CFG_KEEPS_IDS_IN_CLUSTER)
		dnet_ids_update(n, 0, path, cfg_addrs, backend_id);

	ids = reinterpret_cast<struct dnet_raw_id *>(malloc(st.st_size));
	if (!ids) {
		err = -ENOMEM;
		goto err_out_close;
	}

	err = read(fd, ids, st.st_size);
	if (err != st.st_size) {
		err = -errno;
		dnet_log_err(n, "Failed to read ids file '%s'", path);
		goto err_out_free;
	}

	close(fd);

	*id_num = num;
	return ids;

err_out_free:
	free(ids);
err_out_close:
	close(fd);
err_out_exit:
	return NULL;
}

class backend_stat_provider : public ioremap::monitor::stat_provider {
public:
	backend_stat_provider(const dnet_backend_io *backend_io)
	: m_cb(backend_io->cb)
	{}

	static std::string name(uint64_t backend_id)
	{
		return "backend_" + std::to_string(backend_id);
	}

	virtual std::string json() const {
		char *json_stat = NULL;
		size_t size = 0;
		if (m_cb->storage_stat_json)
			m_cb->storage_stat_json(m_cb->command_private, &json_stat, &size);
		return std::string(json_stat, size);
	}

	virtual bool check_category(int category) const {
		return category == DNET_MONITOR_BACKEND || category == DNET_MONITOR_ALL;
	}

private:
	const dnet_backend_callbacks *m_cb;
};

static int dnet_backend_stat_provider_init(struct dnet_backend_io *backend, struct dnet_node *n)
{
	try {
		ioremap::monitor::add_provider(n, new backend_stat_provider(backend), backend_stat_provider::name(backend->backend_id));
	} catch (...) {
		return -ENOMEM;
	}
	return 0;
}

static void dnet_backend_stat_provider_cleanup(size_t backend_id, struct dnet_node *n)
{
	ioremap::monitor::remove_provider(n, backend_stat_provider::name(backend_id));
}

int dnet_backend_init(struct dnet_node *node, size_t backend_id, unsigned *state)
{
	int ids_num;
	struct dnet_raw_id *ids;

	auto &backends = node->config_data->backends->backends;
	if (backends.size() <= backend_id) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, invalid backend id", backend_id);
		return -EINVAL;
	}

	dnet_backend_info &backend = backends[backend_id];

	{
		std::lock_guard<std::mutex> guard(*backend.state_mutex);
		*state = backend.state;
		if (backend.state != DNET_BACKEND_DISABLED) {
			dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, trying to activate not disabled backend", backend_id);
			if (*state == DNET_BACKEND_ENABLED)
				return -EALREADY;
			else if (*state == DNET_BACKEND_ACTIVATING)
				return -EINPROGRESS;
			else /*if (*state == DNET_BACKEND_DEACTIVATING)*/
				return -EAGAIN;
			return -EINVAL;
		}
		backend.state = DNET_BACKEND_ACTIVATING;
	}

	backend.config = backend.config_template;
	backend.data.assign(backend.data.size(), '\0');
	backend.config.data = backend.data.data();
	backend.config.log = backend.log;

	dnet_backend_io *backend_io = &node->io->backends[backend_id];

	for (auto it = backend.options.begin(); it != backend.options.end(); ++it) {
		dnet_backend_config_entry &entry = *it;
		entry.value.assign(entry.value_template.begin(), entry.value_template.end());
		entry.entry->callback(&backend.config, entry.entry->key, entry.value.data());
	}

	int err = backend.config.init(&backend.config);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init backend: %d", backend_id, err);
		goto err_out_exit;
	}

	if (node->cache_size) {
		backend_io->cache = backend.cache = dnet_cache_init(node, backend_io);
		if (!backend.cache) {
			dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init cache, err: %d", backend_id, err);
			goto err_out_backend_cleanup;
		}
	}

	backend_io->cb = &backend.config.cb;

	err = dnet_backend_io_init(node, backend_io);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init io pool, err: %d", backend_id, err);
		goto err_out_cache_cleanup;
	}

	err = dnet_backend_stat_provider_init(backend_io, node);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init stat provider, err: %d", backend_id, err);
		goto err_out_backend_io_cleanup;
	}

	ids_num = 0;
	ids = dnet_ids_init(node, backend.history.c_str(), &ids_num, backend.config.storage_free, node->addrs, backend_id);
	err = dnet_route_list_enable_backend(node->route, backend_id, backend.group, ids, ids_num);
	free(ids);

	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to add backend to route list, err: %d", backend_id, err);
		goto err_out_stat_destroy;
	}

	dnet_log(node, DNET_LOG_INFO, "backend_init: backend: %zu, initialized", backend_id);

	{
		std::lock_guard<std::mutex> guard(*backend.state_mutex);
		dnet_current_time(&backend.last_start);
		backend.last_start_err = 0;
		backend.state = DNET_BACKEND_ENABLED;
	}
	return 0;

	dnet_route_list_disable_backend(node->route, backend_id);
err_out_stat_destroy:
	dnet_backend_stat_provider_cleanup(backend_id, node);
err_out_backend_io_cleanup:
	backend_io->need_exit = 1;
	dnet_backend_io_cleanup(node, backend_io);
	node->io->backends[backend_id].cb = NULL;
err_out_cache_cleanup:
	if (backend.cache) {
		dnet_cache_cleanup(backend.cache);
		backend.cache = NULL;
		backend_io->cache = NULL;
	}
err_out_backend_cleanup:
	backend.config.cleanup(&backend.config);
err_out_exit:
	{
		std::lock_guard<std::mutex> guard(*backend.state_mutex);
		dnet_current_time(&backend.last_start);
		backend.last_start_err = err;
		backend.state = DNET_BACKEND_DISABLED;
	}
	return err;
}

int dnet_backend_cleanup(struct dnet_node *node, size_t backend_id, unsigned *state)
{
	if (backend_id >= node->config_data->backends->backends.size()) {
		return -EINVAL;
	}

	dnet_backend_info &backend = node->config_data->backends->backends[backend_id];

	{
		std::lock_guard<std::mutex> guard(*backend.state_mutex);
		*state = backend.state;
		if (backend.state != DNET_BACKEND_ENABLED) {
			if (*state == DNET_BACKEND_DISABLED)
				return -EALREADY;
			else if (*state == DNET_BACKEND_DEACTIVATING)
				return -EINPROGRESS;
			else /*if (*state == DNET_BACKEND_ACTIVATING)*/
				return -EAGAIN;
		}
		backend.state = DNET_BACKEND_DEACTIVATING;
	}

	dnet_backend_io *backend_io = node->io ? &node->io->backends[backend_id] : NULL;
	if (backend_io)
		backend_io->need_exit = 1;

	if (node->route)
		dnet_route_list_disable_backend(node->route, backend_id);

	dnet_backend_stat_provider_cleanup(backend_id, node);

	if (backend_io)
		dnet_backend_io_cleanup(node, backend_io);

	dnet_cache_cleanup(backend.cache);
	if (backend_io)
		backend_io->cb = NULL;
	backend.cache = NULL;
	backend.config.cleanup(&backend.config);

	{
		std::lock_guard<std::mutex> guard(*backend.state_mutex);
		backend.state = DNET_BACKEND_DISABLED;
	}
	return 0;
}

int dnet_backend_init_all(struct dnet_node *node)
{
	int err = 1;
	unsigned state = DNET_BACKEND_ENABLED;

	auto &backends = node->config_data->backends->backends;
	for (size_t backend_id = 0; backend_id < backends.size(); ++backend_id) {
		dnet_backend_info &backend = node->config_data->backends->backends[backend_id];
		if (!backend.enable_at_start)
			continue;

		int tmp = dnet_backend_init(node, backend_id, &state);
		if (!tmp)
			err = 0;
		else if (err == 1)
			err = tmp;
	}

	return err == 1 ? -EINVAL : err;
}

void dnet_backend_cleanup_all(struct dnet_node *node)
{
	unsigned state = DNET_BACKEND_ENABLED;

	auto &backends = node->config_data->backends->backends;
	for (size_t backend_id = 0; backend_id < backends.size(); ++backend_id) {
		dnet_backend_cleanup(node, backend_id, &state);
	}
}

static void backend_fill_status(dnet_node *node, dnet_backend_status &status, size_t backend_id)
{
	const auto &backends = node->config_data->backends->backends;
	const dnet_backend_info &backend = backends[backend_id];

	std::lock_guard<std::mutex> guard(*backend.state_mutex);

	const auto &cb = backend.config.cb;

	status.backend_id = backend_id;
	status.state = backend.state;
	if (cb.defrag_status)
		status.defrag_state = cb.defrag_status(cb.command_private);
	status.last_start = backend.last_start;
	status.last_start_err = backend.last_start_err;
}

int dnet_cmd_backend_control(struct dnet_net_state *st, struct dnet_cmd *cmd, void *data)
{
	dnet_node *node = st->n;
	const auto &backends = node->config_data->backends->backends;

	if (cmd->size != sizeof(dnet_backend_control)) {
		return -EINVAL;
	}

	struct dnet_backend_control *control = reinterpret_cast<dnet_backend_control *>(data);

	if (control->backend_id >= backends.size()) {
		return -EINVAL;
	}

	const dnet_backend_info &backend = backends[control->backend_id];

	unsigned state = DNET_BACKEND_DISABLED;
	const dnet_backend_callbacks &cb = backend.config.cb;

	int err = 0;
	switch (control->command) {
	case DNET_BACKEND_ENABLE:
		err = dnet_backend_init(st->n, control->backend_id, &state);
		break;
	case DNET_BACKEND_DISABLE:
		err = dnet_backend_cleanup(st->n, control->backend_id, &state);
		break;
	case DNET_BACKEND_START_DEFRAG:
		if (cb.defrag_start) {
			err = cb.defrag_start(cb.command_private);
		} else {
			err = -ENOTSUP;
		}
		break;
	}

	char buffer[sizeof(dnet_backend_status_list) + sizeof(dnet_backend_status)];
	memset(buffer, 0, sizeof(buffer));

	dnet_backend_status_list *list = reinterpret_cast<dnet_backend_status_list *>(buffer);
	dnet_backend_status *status = reinterpret_cast<dnet_backend_status *>(list + 1);

	list->backends_count = 1;
	backend_fill_status(node, *status, control->backend_id);

	if (err) {
		dnet_send_reply(st, cmd, list, sizeof(buffer), true);
	} else {
		cmd->flags &= ~DNET_FLAGS_NEED_ACK;
		err = dnet_send_reply(st, cmd, list, sizeof(buffer), false);
		if (err) {
			cmd->flags |= DNET_FLAGS_NEED_ACK;
			return 0;
		}
	}

	return err;
}

int dnet_cmd_backend_status(struct dnet_net_state *st, struct dnet_cmd *cmd, void *data)
{
	(void) data;
	dnet_node *node = st->n;

	const auto &backends = node->config_data->backends->backends;
	const size_t total_size = sizeof(dnet_backend_status_list) + backends.size() * sizeof(dnet_backend_status);

	std::unique_ptr<dnet_backend_status_list, free_destroyer> list(reinterpret_cast<dnet_backend_status_list *>(malloc(total_size)));
	if (!list) {
		return -ENOMEM;
	}
	memset(list.get(), 0, total_size);

	list->backends_count = backends.size();

	for (size_t i = 0; i < backends.size(); ++i) {
		dnet_backend_status &status = list->backends[i];
		backend_fill_status(st->n, status, i);
	}

	cmd->flags &= ~DNET_FLAGS_NEED_ACK;

	int err = dnet_send_reply(st, cmd, list.get(), total_size, false);

	if (err != 0) {
		cmd->flags |= DNET_FLAGS_NEED_ACK;
	}

	return err;
}