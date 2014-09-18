uwsgi-docker
============

An uWSGI plugin for integration with [Docker](https://www.docker.com/).

This plugin allows you to configure and run vassals in Docker containers.

Once the Docker daemon is started and you have a bunch of images, you only need to edit a vassal's configuration to dockerize it.

Requirements
============

At least uWSGI 2.1 for the Emperor and uWSGI 2.0.6 for vassals is required.

However, for a better experience you should use uWSGI >= 2.1 even in vassals to get advanced features such as binding a socket in the host and passing it to the Docker instance without worrying about redirecting ports or mapping additional filesystems.

To build the plugin you need libcurl (and its development headers) and libjansson (and its development headers).

A little understanding of Docker and how it works is highly suggested.

Only Linux is supported, as it is the only platform supported by Docker.

Quickstart
==========

Build the plugin. The plugin is only required for the Emperor. Vassals are not aware of being dockerized.

```sh
uwsgi --build-plugin https://github.com/unbit/uwsgi-docker
```

Ensure the Docker daemon is running. (For example, run it from the terminal as root with `docker -d`.)

Then prepare your first image. In this example, we'll be preparing a Perl/PSGI app based on Ubuntu.

```sh
docker run -t -i --name=psgi_image ubuntu /bin/bash
```

You are now in a new container called "psgi_image", and you can start installing the required packages. In this case, we install a bunch of [CPAN](http://www.cpan.org/) modules, uWSGI itself and a simple Hello World PSGI app.

```sh
apt-get update
apt-get install python libperl-dev build-essential libpcre3-dev cpanminus
apt-get clean
cpanm Plack
curl http://uwsgi.it/install | bash -s psgi /usr/bin/uwsgi
mkdir /var/www
echo "my \$app = sub { return [200, [], ['Hello World from Docker']];}" > /var/www/app.pl
```

Then we'll commit the image with the name 'psgi001'. Run the following command in another terminal.

```sh
docker commit psgi_image psgi001
```

Now we are free to destroy the "psgi_image" container.

```sh
docker stop psgi_image
docker rm psgi_image
```

The "psgi001" image is ready to be used. Let's prepare a vassal for it...

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; map host port 9001 to container port 3031/tcp
docker-port = 9001:3031
; map host port 127.0.0.1:5001 to container port 5000/tcp
docker-port = 127.0.0.1:5001:5000

[uwsgi]
psgi = /var/www/app.pl
processes = 4
; in bridged mode the class B network is allocated, so we can simply bind to the first address starting with 172
; so we use the very handy .* trick
socket = 172.*:3031
uid = www-data
gid = www-data
; we are free to bind the stats server to all addresses, as Docker will "firewall" it
stats = :5000
```

Then we'll run the Emperor with the Docker plugin.

```ini
[uwsgi]
plugin = path_to/docker_plugin.so
emperor = dir:///etc/uwsgi
; Just loading the plugin does not force Docker support.
; This option disallows running vassals without Docker.
emperor-docker-required = true
; Use /usr/bin/uwsgi as the container entry point (the path could be different from the Emperor one, so we force it)
emperor-wrapper = /usr/bin/uwsgi

```

Simplyfing socket management (requires uWSGI 2.1 in vassals)
============================================================

One of the most common needs in app management is allowing connections from a frontend web proxy to backend applications.
When running under Docker applications run in a different network namespace, so you'll need some way to enable communications between them.

In the quickstart example we saw how to forward ports from the host to the container. 
You can do the same with UNIX sockets by mounting a host directory (where sockets are bound) to the container.

Both are easy to accomplish, but are suboptimal:

* When mounting a directory you will be very probably forced to reveal all of the bound sockets within to the container. This may be a privacy/security problem, especially if you give the sockets a meaningful name.
* When forwarding ports, you need to choose which port is mapped which. This may get cumbersome and may be error-prone and brittle.
* Forwarding ports has its overhead (even if it was all managed in kernel space via iptables).

To this end, the Docker plugin exposes a `docker-socket` attribute, allowing you to bind a socket in the host and automatically pass it to the Docker instance. Our previous example can be simplified in this way:

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
```

The host binds to `/var/run/example.com.socket` and passes it to the dockerized vassal automatically.

Now all you only need to configure your proxy (Nginx, Apache, etc.) to connect to `/var/run/example.com.socket`.

You can bind to both UNIX and INET sockets.

Remember that you need uWSGI 2.1 in the vassal to support socket passing (though it is probable this feature will be backported to uWSGI 2.0.8).

How it works
============

The plugin communicates with the Docker daemon UNIX socket (`/var/run/docker.sock`).

On vassal start, the plugin asks the Docker daemon to create a new container with specific attributes and with same name of the vassal (that is, a vassal by the name of "foobar.ini" will generate a Docker container called "foobar.ini"). 
This container automatically gets access to an UNIX socket (the proxy Emperor socket) through which it will receive the Emperor's file descriptors (control pipe, configuration pipe and possibly the on-demand socket).
Once the Emperor proxy has passed the file descriptors it is completely destroyed, as from now on the Emperor has full control over the dockerized instance.

For security and robustness the Docker transaction is managed by a separate process for each vassal. This process is named [uwsgi-docker-bridge] and it requires very little memory. Once this process has completed the spawn of the instance itself, it attaches to the pseudoterminal of the Docker instance, to let you transparently get instance logs if they are not redirected. Once the pseudoterminal closes, the vassal is destroyed too.

During the startup phase of the vassal, if an existing Docker instance named as the vassal is found, it will be automatically destroyed.

When the emperor dies, all of the related containers are destroyed too.

The Emperor Proxy
=================

As we saw, the Dockerized instance requires access to a special UNIX socket (the emperor proxy socket) that will pass all of the required file descriptors to the vassal.

By the default this socket is created in the same directory of the vassal file suffixing it with ".sock".

That is, "/etc/uwsgi/foobar.ini" will generate "/etc/uwsgi/foobar.ini.sock" that will be mounted as "/foobar.ini.sock" in the container.

You can override this behavior with the `docker-proxy` attribute.

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket
docker-proxy=/var/run/foobar.sock:/tmp/foosocket

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
```

This will create /var/run/foobar.sock in the host, exposed to the docker instance as /tmp/foosocket.

Remember that the UNIX socket file is removed from the host as soon as the Docker instance has connected to it.

An important thing **YOU HAVE TO REMEMBER**, is that the uWSGI process run by the instance must have write access to this socket, so if you are using the `docker-user` attribute to start uWSGI as an unprivileged user instead of configuring it for dropping privileges, you will have to tell the emperor to create sockets with a specific permission mode using the classic `chmod-socket` option.

```ini
[uwsgi]
plugin = path_to/docker_plugin.so
emperor = dir:///etc/uwsgi
; once the plugin is loaded, docker support is optional
; this option disallow running vassals without docker
emperor-docker-required = true
; use /usr/bin/uwsgi as the container entry point (the path coudl be different from the Emperor one, so we force it)
emperor-wrapper = /usr/bin/uwsgi
chmod-socket = 666
```

Remember the socket is suddenly destroyed after the first connection.


Integration with the forkpty router plugin
==========================================

[The forkpty router](http://uwsgi-docs.readthedocs.org/en/latest/ForkptyRouter.html) is a plugin included in the official uWSGI sources. It allows you to attach a terminal session to a running uWSGI instance.

The best approach is exposing a host directory for the vassal in the Docker container.
You will be able to use this directory to create the UNIX socket of the forkpty router. (Another approach would be binding it on a inet socket and forwarding the port.)

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

; mount /pty/foo as /foo (ensure it is owned by www-data)
docker-mount = /pty/foo:/pty

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
forkpty-router = /pty/socket
```

Now you can enter the container with:

```sh
uwsgi --pty-connect /pty/foo/socket
```

Remember to rebuild uWSGI with `pty` and `forkptyrouter` plugins embedded. You can do this quickly with

```sh
UWSGI_EMBED_PLUGINS=pty,forkptyrouter make psgi
```

Technically the host instance only requires `pty`, while the dockerized only requires `forkptyrouter`.

You can of course also build them as external plugins and load them dynamically.

```sh
uwsgi --build-plugin plugins/pty
uwsgi --build-plugin plugins/forkptyrouter
```

By default the forkptyrouter runs `/bin/sh`. `/bin/bash` is likely a better choice -- you may set it with `forkptyrouter-command` option.

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

; mount /pty/foo as /foo (ensure it is owned by www-data)
docker-mount = /pty/foo:/pty

[uwsgi]
plugin = path_to/forkptyrouter_plugin.so
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
forkpty-router = /pty/socket
forkptyrouter-command = /bin/bash
```

The forkptyrouter can be used instead of `nsenter` as it does not require special privileges (your client needs only write access to the specific UNIX socket).

Attributes
==========

* `docker-image` -- set the image to use for the container **(REQUIRED)**
* `docker-socket` -- bind a socket and pass it to the Docker instance
* `docker-port` -- forward a port, syntax `hostip:hostport:dockerport` or `hostport:dockerport`
* `docker-workdir` -- set working directory
* `docker-mount` -- bind mount from host to container, syntax `/host:/docker`
* `docker-network-mode` -- set network mode (bridge, host, none, container:id)
* `docker-hostname` -- set container hostname
* `docker-proxy` -- set emperor proxy path, syntax `hostpath:dockerpath`
* `docker-env` -- set an environment variable in the container, syntax `NAME=VALUE`
* `docker-user` -- run uWSGI in the container as the specified user (otherwise it will start as root and you will need to specify uid and gid in the vassal)
* `docker-memory` -- set the max amount of memory (in bytes) for the container
* `docker-swap` -- set the max amount of swap memory (in bytes) for the container
* `docker-cidfile` -- store the cid (Container ID) file in the specified path
* `docker-dns` -- add a DNS server to the container

Options
=======

* `--docker-emperor/--emperor-docker` -- enable Docker support in the Emperor
* `--docker-emperor-required/--emperor-docker-required` -- enable Docker support in the Emperor and require each vassal to expose Docker options
* `--docker-debug` -- enable debug logging
* `--docker-daemon-socket` -- change the default Docker daemon socket (default `/var/run/docker.sock`)

Known Issues
============

* Only .ini files are supported (must be fixed in uWSGI itself)
* Currently configs are piped via the emperor proxy socket, but allowing the dockerized instance to access the original file should be permitted (now config piping is hardcoded)
