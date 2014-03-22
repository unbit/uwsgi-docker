uwsgi-docker
============

uWSGI plugin for integration with Docker

Why ?
=====

uWSGI implements a set of advanced features for linux namespaces and cgroups that combined with the Emperor offer a very powerful way for creating PaaS and hosting systems.

Docker (http://docker.io) has its "containers way" built on top of lxc (that not necessarely applies well to some of the scenarios covered by uWSGI) but implements an amazing way for managing filesystem images.

The plugin tries to combine uWSGI containers features with docker images.

Should i use it ?
=================

As always, it depends. If you already have an infrastructure based on uWSGI (and its namespace features), introducing support for Docker images will be very useful for your developers, as they can simplify their deployment (all became a matter of committing and pushing new images) and get access to hundreds of ready-to-go application stacks.

On the contrary, if you already have a Docker-based infrastructure, probably you'd better to continue using uWSGI as the application server


How it works
============

By default you can set the rootfs of an instance/vassal with a single option:

```ini
[uwsgi]
plugins = docker
socket = /var/run/myapp.socket
docker-image = ubuntu
wsgi-file = /var/www/myapp.py
```

Under the hood the following steps happen:

- the instance connects to the unix socket /var/run/docker.sock
- the instance sends a "GET /v1.10/info" to the socket and retrieve the "Driver" and "DriverStatus" attributes.
- the instance sends a "GET /v1.10/images/json" and search for the tag "ubuntu" in the images list
- if the image is found, the graph of the parents is retrieved from the images list
- the instance unshare() the mount namespace with CLONE_NEWNS
- the instance creates a temporary directory as the rw part of the filesystem stack (it is regenerated at every run, unless you specify it with the ``docker-image-rwpath`` option)
- the instance mount() the aufs (or overlayfs) tree using the previously generated graph (getting the real paths thanks to the "Root Dir" attribute available in "DriverStatus"
- the instance call pivot_root() for setting the filesystem graph as the new rootfs
- additional filesystems (/proc, /sys/, /dev/pts...) are mounted accordingly (this is configurable)
- the instance continues its normal startup procedure
