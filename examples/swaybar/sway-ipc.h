#if !defined(SWAY_IPC_H)
#define SWAY_IPC_H

#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include "macros.h"

// TODO: nonblock read/write

enum sway_ipc_message_type {
	// i3 command types - see i3's I3_REPLY_TYPE constants
	SWAY_IPC_MESSAGE_TYPE_COMMAND = 0,
	SWAY_IPC_MESSAGE_TYPE_GET_WORKSPACES = 1,
	SWAY_IPC_MESSAGE_TYPE_SUBSCRIBE = 2,
	SWAY_IPC_MESSAGE_TYPE_GET_OUTPUTS = 3,
	SWAY_IPC_MESSAGE_TYPE_GET_TREE = 4,
	SWAY_IPC_MESSAGE_TYPE_GET_MARKS = 5,
	SWAY_IPC_MESSAGE_TYPE_GET_BAR_CONFIG = 6,
	SWAY_IPC_MESSAGE_TYPE_GET_VERSION = 7,
	SWAY_IPC_MESSAGE_TYPE_GET_BINDING_MODES = 8,
	SWAY_IPC_MESSAGE_TYPE_GET_CONFIG = 9,
	SWAY_IPC_MESSAGE_TYPE_SEND_TICK = 10,
	SWAY_IPC_MESSAGE_TYPE_SYNC = 11,
	SWAY_IPC_MESSAGE_TYPE_GET_BINDING_STATE = 12,

	// sway-specific message types
	SWAY_IPC_MESSAGE_TYPE_GET_INPUTS = 100,
	SWAY_IPC_MESSAGE_TYPE_GET_SEATS = 101,

	// Events sent from sway to clients. Events have the highest bits set.
	SWAY_IPC_MESSAGE_TYPE_EVENT_WORKSPACE = -2147483648,
	SWAY_IPC_MESSAGE_TYPE_EVENT_OUTPUT = -2147483647,
	SWAY_IPC_MESSAGE_TYPE_EVENT_MODE = -2147483646,
	SWAY_IPC_MESSAGE_TYPE_EVENT_WINDOW = -2147483645,
	SWAY_IPC_MESSAGE_TYPE_EVENT_BARCONFIG_UPDATE = -2147483644,
	SWAY_IPC_MESSAGE_TYPE_EVENT_BINDING = -2147483643,
	SWAY_IPC_MESSAGE_TYPE_EVENT_SHUTDOWN = -2147483642,
	SWAY_IPC_MESSAGE_TYPE_EVENT_TICK = -2147483641,

	// sway-specific event types
	SWAY_IPC_MESSAGE_TYPE_EVENT_BAR_STATE_UPDATE = -2147483628,
	SWAY_IPC_MESSAGE_TYPE_EVENT_INPUT = -2147483627,
};

struct sway_ipc_response {
	uint32_t length;
	enum sway_ipc_message_type type;
	char *payload;
};

static const char sway_ipc_magic[] = {'i', '3', '-', 'i', 'p', 'c'};

#define SWAY_IPC_HEADER_SIZE (sizeof(sway_ipc_magic) + 8)

static MAYBE_UNUSED char *sway_ipc_get_socket_path(void) {
	const char *swaysock = getenv("SWAYSOCK");
	if (swaysock) {
		return strdup(swaysock);
	}
	char *line = NULL;
	size_t line_size = 0;
	FILE *fp = popen("sway --get-socketpath 2>/dev/null", "r");
	if (fp) {
		ssize_t nret = getline(&line, &line_size, fp);
		pclose(fp);
		if (nret > 0) {
			if (line[nret - 1] == '\n') {
				line[nret - 1] = '\0';
			}
			return line;
		}
	}
	const char *i3sock = getenv("I3SOCK");
	if (i3sock) {
		free(line);
		return strdup(i3sock);
	}
	fp = popen("i3 --get-socketpath 2>/dev/null", "r");
	if (fp) {
		ssize_t nret = getline(&line, &line_size, fp);
		pclose(fp);
		if (nret > 0) {
			if (line[nret - 1] == '\n') {
				line[nret - 1] = '\0';
			}
			return line;
		}
	}
	free(line);
	return NULL;
}

static MAYBE_UNUSED int sway_ipc_connect(const char *socket_path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		return -1;
	}
	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	if (connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
		return -1;
	}
	return fd;
}

static MAYBE_UNUSED int sway_ipc_send(int fd, enum sway_ipc_message_type type,
		const char *payload, uint32_t payload_length) {
	char header[SWAY_IPC_HEADER_SIZE];
	memcpy(header, sway_ipc_magic, sizeof(sway_ipc_magic));
	memcpy(header + sizeof(sway_ipc_magic), &payload_length, sizeof(payload_length));
	memcpy(header + sizeof(sway_ipc_magic) + sizeof(payload_length), &type, sizeof(type));

	size_t total = 0;
	while (total < sizeof(header)) {
		ssize_t written_bytes = write(fd, header + total, sizeof(header) - total);
		if (written_bytes == -1) {
			return -1;
		}
		total += (size_t)written_bytes;
	}

	total = 0;
	while (total < payload_length) {
		ssize_t written_bytes = write(fd, payload + total, payload_length - total);
		if (written_bytes == -1) {
			return -1;
		}
		total += (size_t)written_bytes;
	}

	return 1;
}

static MAYBE_UNUSED void sway_ipc_response_free(struct sway_ipc_response *response) {
	if (response == NULL) {
		return;
	}

	free(response->payload);
	free(response);
}

static MAYBE_UNUSED struct sway_ipc_response *sway_ipc_receive(int fd) {
	char header[SWAY_IPC_HEADER_SIZE];
	size_t total = 0;
	while (total < sizeof(header)) {
		ssize_t read_bytes = read(fd, header + total, sizeof(header) - total);
		switch (read_bytes) {
		case 0:
			errno = EPIPE;
			ATTRIB_FALLTHROUGH;
		case -1:
			if (errno == EINTR) {
				continue;
			}
			return NULL;
		default:
			total += (size_t)read_bytes;
		}
	}

	struct sway_ipc_response *response = malloc(sizeof(struct sway_ipc_response));
	memcpy(&response->length, header + sizeof(sway_ipc_magic), sizeof(uint32_t));
	memcpy(&response->type, header + sizeof(sway_ipc_magic) + sizeof(uint32_t), sizeof(uint32_t));

	response->payload = malloc(response->length + 1);
	total = 0;
	while (total < response->length) {
		ssize_t read_bytes = read(fd, response->payload + total, response->length - total);
		switch (read_bytes) {
		case 0:
			errno = EPIPE;
			ATTRIB_FALLTHROUGH;
		case -1:
			if (errno == EINTR) {
				continue;
			}
			sway_ipc_response_free(response);
			return NULL;
		default:
			total += (size_t)read_bytes;
		}
	}

	response->payload[response->length] = '\0';
	return response;
}

#endif // SWAY_IPC_H
