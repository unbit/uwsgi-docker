#include <uwsgi.h>
#include <curl/curl.h>
#include <jansson.h>

extern struct uwsgi_server uwsgi;

#define DOCKER_SOCKET "/var/run/docker.sock"
#define DOCKER_API "1.14"

static struct uwsgi_docker {
	int emperor;
	int debug;
	int emperor_required;
	char *socket;
} udocker;

static struct uwsgi_option docker_options[] = {
	{"docker-emperor", no_argument, 0, "enable Emperor integration with docker", uwsgi_opt_true, &udocker.emperor, 0},
	{"emperor-docker", no_argument, 0, "enable Emperor integration with docker", uwsgi_opt_true, &udocker.emperor, 0},
	{"docker-emperor-required", no_argument, 0, "enable Emperor integration with docker", uwsgi_opt_true, &udocker.emperor_required, 0},
	{"emperor-docker-required", no_argument, 0, "enable Emperor integration with docker", uwsgi_opt_true, &udocker.emperor_required, 0},
	{"docker-debug", no_argument, 0, "enable debug mode", uwsgi_opt_true, &udocker.debug, 0},
	{"docker-daemon-socket", required_argument, 0, "set the docker daemon socket path (default: " DOCKER_SOCKET ")", uwsgi_opt_set_str, &udocker.socket, 0},
	UWSGI_END_OF_OPTIONS
};

// hack for adding support for unix sockets to libcurl
static curl_socket_t docker_unix_socket(void *foobar, curlsocktype cs_type, struct curl_sockaddr *c_addr) {
	struct sockaddr_un* un_addr = (struct sockaddr_un*)&c_addr->addr;
	c_addr->family = AF_UNIX;
	c_addr->addrlen = sizeof(struct sockaddr_un);

	memset(un_addr, 0, c_addr->addrlen);
	un_addr->sun_family = AF_UNIX;
	strncpy(un_addr->sun_path, udocker.socket, sizeof(un_addr->sun_path));
	c_addr->protocol = 0;
	return socket(c_addr->family, c_addr->socktype, c_addr->protocol);
}

// store a libcurl response in a uwsgi_buffer
static size_t docker_response(void *ptr, size_t size, size_t nmemb, void *data) {
	struct uwsgi_buffer *ub = (struct uwsgi_buffer *) data;
	if (uwsgi_buffer_append(ub, ptr, size*nmemb)) return -1;
	return size*nmemb;
}

static json_t *docker_json(char *method, char *url, json_t *json, long *http_status) {
	json_t *response = NULL;
	char *json_body = NULL;
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	CURL *curl = curl_easy_init();
	if (!curl) goto error;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, uwsgi.socket_timeout);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, uwsgi.socket_timeout);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	char *full_url = uwsgi_concat2("http://127.0.0.1", url);
	curl_easy_setopt(curl, CURLOPT_URL, full_url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (json) {
		json_body = json_dumps(json, 0);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	}
	curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, docker_unix_socket);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, docker_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ub);
	CURLcode res = curl_easy_perform(curl);
	if (json_body) free(json_body);

	if (res != CURLE_OK) {
		uwsgi_log("[docker] error sending request %s: %s\n", full_url, curl_easy_strerror(res));
		free(full_url);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		goto error;
	}

	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, http_status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (udocker.debug) {
		uwsgi_log("[docker-debug] HTTP request to %s -> %d\n%.*s\n", full_url, *http_status, ub->pos, ub->buf);
	}

	free(full_url);

	json_error_t error;
	response = json_loadb(ub->buf, ub->pos, 0, &error);
error:
	uwsgi_buffer_destroy(ub);
	return response;
}

// check if a container has teh specified name
static int docker_hasname(json_t *obj, char *container_name) {
	json_t *names = json_object_get(obj, "Names");
	if (!names || !json_is_array(names)) {
		return 0;
	}
	size_t i, items = json_array_size(names);
	for(i=0;i<items;i++) {
		json_t *name = json_array_get(names, i);
		if (!name || !json_is_string(name)) continue;
		char *value = (char *)json_string_value(name);
		if (!value) continue;
		if (value[0] == '/') {
			if (!strcmp(container_name, value+1)) {
				return 1;
			}
		}
		else {
			if (!strcmp(container_name, value)) {
				return 1;
			}
		}
	}
	return 0;
}

// stop and DELETE
static int docker_destroy(char *name, char *container_id) {
	// useful for menaging jansson reference counting
	json_t *garbage = NULL;
	long http_status = 0;
	// get the container_id by its name
	if (!container_id) {
		json_t *response = docker_json("GET", "/containers/json?all=1", NULL, &http_status);
		if (!response || !json_is_array(response)) {
			uwsgi_log("[docker] unable to get containers list\n");
                	return -1;
		}
		size_t i, items = json_array_size(response);
		for(i=0;i<items;i++) {
			json_t *container_object = json_array_get(response, i);
			if (!container_object || !json_is_object(container_object)) {
				uwsgi_log("[docker] invalid containers list\n");
                		return -1;
			}
			if (docker_hasname(container_object, name)) {
				uwsgi_log("CONTAINER FOUND !!\n");
				json_t *json_container_id = json_object_get(container_object, "Id");
				if (!json_container_id || !json_is_string(json_container_id)) {
					uwsgi_log("[docker] unable to get container id for %s\n", name);
                			return -1;
				}
				container_id = (char *) json_string_value(json_container_id);
				break;
			}
		}
		garbage = response;
	}

	if (!container_id) {
		uwsgi_log("[docker] unable to get container id for %s\n", name);
		if (garbage) json_decref(garbage);
                return -1;
	}

	char *url = uwsgi_concat3("/containers/", container_id, "/stop?t=3");
	json_t *response = docker_json("POST", url, NULL, &http_status);
	free(url);
	if (response) json_decref(response);
	if (http_status != 204 && http_status != 304) {
		if (garbage) json_decref(garbage);
		uwsgi_log("[docker] unable to stop container %s\n", container_id);
		return -1;
	}
	uwsgi_log("[docker] container %s stopped\n", container_id);

	// now DELETE
	url = uwsgi_concat2("/containers/", container_id);
	response = docker_json("DELETE", url, NULL, &http_status);
	free(url);
	if (response) json_decref(response);
	if (http_status != 204) {
		if (garbage) json_decref(garbage);
		uwsgi_log("[docker] unable to delete container %s\n", container_id);
		return -1;
	}

	uwsgi_log("[docker] container %s deleted\n", container_id);
	if (garbage) json_decref(garbage);
	return 0;
}



// here we use a raw connection
static void docker_attach(struct uwsgi_instance *ui, int proxy_fd, char *proxy_path, char *container_id, int socket_fd) {

	uwsgi_log("[docker] waiting for proxy connection on container %s (%s)\n", container_id, ui->name);
	// wait for connection
	uwsgi_master_manage_emperor_proxy(proxy_fd, ui->pipe[1], ui->pipe_config[1], socket_fd);
	// we do not need those fds anymore
	close(proxy_fd);
	close(ui->pipe[1]);
	if (ui->pipe_config[1] > -1)
		close(ui->pipe_config[1]);
	if (socket_fd > -1)
		close(socket_fd);
	unlink(proxy_path);

	int fd = uwsgi_connect(udocker.socket, uwsgi.socket_timeout, 0);
	if (fd < 0) goto end;

	// send a raw request, we do not need curl this time
	// ub structure us not freed on error, as we brutally exit
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	if (uwsgi_buffer_append(ub, "POST /containers/", 17)) goto end;
	if (uwsgi_buffer_append(ub, container_id, strlen(container_id))) goto end;
	if (uwsgi_buffer_append(ub, "/attach?stream=1&logs=1&stdin=1&stdout=1&stderr=1 HTTP/1.1\r\n\r\n", 62)) goto end;
	if (uwsgi_write_nb(fd, ub->buf, ub->pos, uwsgi.socket_timeout)) {
		uwsgi_error("docker_attach()/write()");
		goto end;
	}
	uwsgi_buffer_destroy(ub);

	// now start waiting for pty data
	for(;;) {
		int ret = uwsgi_waitfd_event(fd, -1, POLLIN);
		if (ret <= 0) break;
		char buf[8192];
		ssize_t rlen = read(fd, buf, 8192);
		if (rlen <= 0) {
			uwsgi_error("docker_attach()/read()");
			break;
		}
		// forward the log line
		uwsgi_log("%.*s", rlen, buf);
	}
end:
	// destroy the container
	uwsgi_log("[docker] destroying container %s (%s) ...\n", container_id, ui->name);
	docker_destroy(ui->name, container_id);
	// never here
}

static int  docker_add_item_to_array(struct uwsgi_instance *ui, char *value, void *data) {
	json_t *array = (json_t *) data;
	json_array_append(array, json_string(value));
	return 0;
}

static int docker_add_port(struct uwsgi_instance *ui, char *value, void *data) {
	int ret = -1;
	json_t *ports = (json_t *) data;
	// how many colons ?
	size_t i,n = 0;
	char **items = uwsgi_split_quoted(value, strlen(value), ":", &n);
	if (n < 2) goto end;
	json_t *port_map_list = json_array();
	json_t *port_map = json_object();
	// hostport:dockerport ?
	char *docker_port = NULL;
	if (n == 2) {
		docker_port = items[1];
		json_object_set(port_map, "HostPort", json_string(items[0]));
	}
	// hostip:hostport:dockerport
	else {
		docker_port = items[2];
		json_object_set(port_map, "HostIp", json_string(items[0]));
		json_object_set(port_map, "HostPort", json_string(items[1]));
	}
	json_array_append(port_map_list, port_map);
	json_object_set(ports, docker_port, port_map_list);
	ret = 0;
end:
	for(i=0;i<n;i++) {
		free(items[i]);
	}
	free(items);
	return ret;
}

static int docker_expose_port(struct uwsgi_instance *ui, char *value, void *data) {
        int ret = -1;
        json_t *ports = (json_t *) data;
        // how many colons ?
        size_t i,n = 0;
        char **items = uwsgi_split_quoted(value, strlen(value), ":", &n);
        if (n < 2) goto end;
        // hostport:dockerport ?
        char *docker_port = NULL;
        if (n == 2) {
                docker_port = items[1];
        }
        // hostip:hostport:dockerport
        else {
                docker_port = items[2];
        }
        json_object_set(ports, docker_port, json_object());
        ret = 0;
end:
        for(i=0;i<n;i++) {
                free(items[i]);
        }
        free(items);
        return ret;
}

// POST /containers/create HTTP/1.1
static void docker_run(struct uwsgi_instance *ui, char **argv) {

	if (!udocker.emperor) return;

	char *proxy_attr = vassal_attr_get(ui, "docker-proxy");
	char *image_attr = vassal_attr_get(ui, "docker-image");

	if (!image_attr) {
		uwsgi_log("[docker] no image attribute specified for vassal %s\n", ui->name);
		if (udocker.emperor_required) {
			exit(1);
		}
		return;
	}

	char *processname = uwsgi_concat2("[uwsgi-docker-bridge] ", ui->name);
	uwsgi_set_processname(processname);
	free(processname);

	char *proxy_attr_emperor = NULL;
	char *proxy_attr_docker = NULL;
	if (proxy_attr) {
		char *colon = strchr(proxy_attr, ':');
		if (colon) {
			*colon = 0;
			// we leak this, sorry :)
			proxy_attr_emperor = uwsgi_str(proxy_attr);
			*colon = ':';
			proxy_attr_docker = colon+1;
		}
		else {
			proxy_attr_emperor = proxy_attr;
			proxy_attr_docker = proxy_attr;
		}
	}
	else {
		// if docker-proxy is not specified we place the socket
		// in the vassals dir
		char *socket_path = uwsgi_expand_path(ui->name, strlen(ui->name), NULL);
		if (!socket_path) {
			uwsgi_log("[docker] unable to build proxy socket path\n");
			exit(1);
		}
		proxy_attr_emperor = uwsgi_concat2(socket_path, ".sock");
		free(socket_path);
		proxy_attr_docker = uwsgi_concat3("/", ui->name, ".sock");
	}

	int socket_fd = -1;
	char *docker_socket = vassal_attr_get(ui, "docker-socket");
	if (docker_socket) {
		char *tcp_port = strchr(docker_socket, ':');
                if (tcp_port) {
                        socket_fd = bind_to_tcp(docker_socket, uwsgi.listen_queue, tcp_port);
                }
                else {
                        socket_fd = bind_to_unix(docker_socket, uwsgi.listen_queue, uwsgi.chmod_socket, 0);
                }
        if (socket_fd == -1) {
        	uswgi_error("error binding docker-socket");
        	exit(1);
        }
	}

	// first of all we wait for proxy connection
        int proxy_fd = bind_to_unix(proxy_attr_emperor, uwsgi.listen_queue, uwsgi.chmod_socket, 0);
        if (proxy_fd < 0) exit(1);

	// start connecting to the docker server in sync way
	json_t *root = json_object();
	if (!root) exit(1);

	if (json_object_set(root, "Image", json_string(image_attr))) exit(1);

	if (json_object_set(root, "AttachStdin", json_true())) exit(1);
	if (json_object_set(root, "OpenStdin", json_true())) exit(1);
	if (json_object_set(root, "Tty", json_true())) exit(1);
	if (json_object_set(root, "AttachStdout", json_true())) exit(1);
	if (json_object_set(root, "AttachStderr", json_true())) exit(1);

	char *docker_workdir = vassal_attr_get(ui, "docker-workdir");
	if (docker_workdir) {
		if (json_object_set(root, "WorkingDir", json_string(docker_workdir))) exit(1);
	}

	char *docker_hostname = vassal_attr_get(ui, "docker-hostname");
	if (docker_hostname) {
		if (json_object_set(root, "Hostname", json_string(docker_hostname))) exit(1);
	}

	char *docker_memory = vassal_attr_get(ui, "docker-memory");
	if (docker_memory) {
		if (json_object_set(root, "Memory", json_integer(strtoul(docker_memory, NULL, 10)))) exit(1);
	}

	char *docker_swap = vassal_attr_get(ui, "docker-swap");
	if (docker_swap) {
		if (json_object_set(root, "MemorySwap", json_integer(strtoul(docker_swap, NULL, 10)))) exit(1);
	}

	char *docker_user = vassal_attr_get(ui, "docker-user");
        if (docker_user) {
                if (json_object_set(root, "User", json_string(docker_user))) exit(1);
        }

	// Env
	json_t *env = json_array();
	if (vassal_attr_get_multi(ui, "docker-env", docker_add_item_to_array, env)) {
                uwsgi_log("[docker] unable to build envs list for vassal %s\n", ui->name);
                exit(1);
        }
	char *env_proxy = uwsgi_concat2("UWSGI_EMPEROR_PROXY=", proxy_attr_docker);
	json_array_append(env, json_string(env_proxy));
	free(env_proxy);
	if (json_object_set(root, "Env", env)) exit(1);

	json_t *ports = json_object();
        if (vassal_attr_get_multi(ui, "docker-port", docker_expose_port, ports)) {
                uwsgi_log("[docker] unable to build port mapping for vassal %s\n", ui->name);
                exit(1);
        }
        if (json_object_set(root, "ExposedPorts", ports)) exit(1);

	json_t *cmd = json_array();
	if (!cmd) exit(1);

	char *arg = *argv++;
	while(arg) {
		json_array_append(cmd, json_string(arg));
		arg = *argv++;
	}

	if (json_object_set(root, "Cmd", cmd)) exit(1);

	char *container_id = NULL;
	json_t *garbage = NULL;

	for(;;) {
		long http_status = 0;

		char *url = uwsgi_concat2("/containers/create?name=", ui->name);
		if (udocker.debug) {
			uwsgi_log("[docker-debug] POST %s\n%s\n", url, json_dumps(root, 0));
		}
		json_t *response = docker_json("POST", url, root, &http_status);
		free(url);

		// if the status code is 409 (Conflict), the container is already running,
		// destroy it and retry
		if (http_status == 409) {
			// if we cannot destroy the container
			// we better to quit
			if (docker_destroy(ui->name, NULL)) {
				exit(1);
			}
			// re-create it
			if (response) json_decref(response);
			continue;
		}

		if (http_status != 201) {
			uwsgi_log("[docker] unable to create container for vassal %s\n", ui->name);
                        exit(1);
		}

		if (!response) exit(1);

		json_t *json_container_id = json_object_get(response, "Id");
		if (!json_container_id) {
			uwsgi_log("[docker] cannot find container id for vassal %s\n", ui->name);
			exit(1);
		}
		if (!json_is_string(json_container_id)) {
			uwsgi_log("[docker] invalid container id for vassal %s\n", ui->name);
                        exit(1);
		}

		container_id = (char *) json_string_value(json_container_id);
		if (!container_id) {
			uwsgi_log("[docker] invalid container id for vassal %s\n", ui->name);
                        exit(1);
		}

		garbage = response;
		break;
	}

	char *docker_cidfile = vassal_attr_get(ui, "docker-cidfile");
	if (docker_cidfile) {
		FILE *cidfile = fopen(docker_cidfile, "w");
		if (!cidfile) {
			uwsgi_error_open(docker_cidfile);
			exit(1);
		}
		if (fprintf(cidfile, "%s\n", container_id) < 2) {
			uwsgi_error("docker_run()/fprintf()");
			exit(1);
		}
		fclose(cidfile);
	}

	// free json object
	json_decref(root);
	// and now we sart the docker instance
	root = json_object();

	// Volumes/Binds
	json_t *binds = json_array();
        if (vassal_attr_get_multi(ui, "docker-mount", docker_add_item_to_array, binds)) {
                uwsgi_log("[docker] unable to build volumes mapping for vassal %s\n", ui->name);
                exit(1);
        }
	char *proxy_bind = uwsgi_concat3(proxy_attr_emperor, ":", proxy_attr_docker);
	json_array_append(binds, json_string(proxy_bind));
	free(proxy_bind);
	json_object_set(root, "Binds", binds);

	// Dns
	json_t *dns = json_array();
	if (vassal_attr_get_multi(ui, "docker-dns", docker_add_item_to_array, dns)) {
                uwsgi_log("[docker] unable to build dns list for vassal %s\n", ui->name);
                exit(1);
        }
	json_object_set(root, "Dns", dns);


	// PortBindings
	ports = json_object();
        if (vassal_attr_get_multi(ui, "docker-port", docker_add_port, ports)) {
                uwsgi_log("[docker] unable to build port mapping for vassal %s\n", ui->name);
                exit(1);
        }
        if (json_object_set(root, "PortBindings", ports)) exit(1);

	char *docker_network_mode = vassal_attr_get(ui, "docker-network-mode");
	if (docker_network_mode) {
                if (json_object_set(root, "NetworkMode", json_string(docker_network_mode))) exit(1);
        }

	long http_status = 0;
	char *url = uwsgi_concat3("/containers/", container_id, "/start");
	if (udocker.debug) {
		uwsgi_log("[docker-debug] POST %s\n%s\n", url, json_dumps(root, 0));
	}
	json_t *response = docker_json("POST", url, root, &http_status);
	free(url);
	if (http_status != 204) {
		uwsgi_log("[docker] unable to start container %s (%s)\n", container_id, ui->name);
		exit(1);
	}

	if (response) json_decref(response);

	// if garbage is defined, container_id is part of a json object
	// this may seems useless, but we want to have the minimal impact
	// for the docker bridge
	if (garbage) {
		container_id = uwsgi_str(container_id);
		json_decref(garbage);
	}

	uwsgi_log("[docker] started %s (%d)\n", container_id, http_status);

	// free json object
	json_decref(root);

	// now attach to stdout and stderr (read: pty)
	docker_attach(ui, proxy_fd, proxy_attr_emperor, container_id, socket_fd);
	// never here ?
	exit(0);
}

static void docker_setup(int (*start)(void *), char **argv) {
	if (udocker.emperor_required) udocker.emperor = 1;
	if (udocker.emperor) {
		// TODO this should be an option
		uwsgi.emperor_force_config_pipe = 1;
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-proxy");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-image");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-port");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-socket");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-workdir");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-hostname");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-memory");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-swap");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-mount");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-dns");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-env");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-user");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-cidfile");
		uwsgi_string_new_list(&uwsgi.emperor_collect_attributes, "docker-network-mode");
	}
	if (!udocker.socket) {
		udocker.socket = DOCKER_SOCKET;
	}
}

struct uwsgi_plugin docker_plugin = {
	.name = "docker",
	.options = docker_options,
	// jail is the best hook to set options before the Emperor spawns
	.jail = docker_setup,
	.vassal_before_exec = docker_run,
};
