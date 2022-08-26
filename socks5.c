
/*
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
*/

uint8_t num_methods = 0;

// Address type struct
typedef struct {
    uint8_t atype;
    uint8_t *addr;
    uint8_t addr_size;
    uint16_t port;
} socks5_address;

#include "common.c"

uint8_t *get_requested_auth_methods(int sock) {
    if ( !read_check(sock, &num_methods, 1, "read num_methods") ) return NULL;

    uint8_t *methods = malloc(num_methods);
    if ( !read_check(sock, methods, num_methods, "read methods") ) {
        free(methods);
        return NULL;
    }
    return methods;
}

bool authenticate_user(int sock, uint8_t *methods) {
    uint8_t response[2] = {0x05, 0x00};
    int n;

    // Only allowing no authentication for now
    for (int i = 0; i < num_methods; i++) {
        if (methods[i] == 0) {
            if ( !write_check(sock, response, 2, "write method authenticate_user") ) return false;
            return true;
        }
    }
    response[1] = 0xFF;
    write_check(sock, response, 2, "write method authenticate_user");
    return false;
}

socks5_address *read_socks5_address(int sock) {
    int n;
    socks5_address *addr = malloc(sizeof(socks5_address));
    if ( !read_check(sock, &addr->atype, 1, "read atype") ) return NULL;

    switch (addr->atype) {
        case 1:
            addr->addr = malloc(4);
            if ( !read_check(sock, addr->addr, 4, "read ip") ) {
                free(addr->addr);
                free(addr);
                return NULL;
            }
            break;
        case 3:
            if ( !read_check(sock, &addr->addr_size, 1, "read addr_size") ) {
                free(addr);
                return NULL;
            }
            addr->addr = malloc(addr->addr_size);
            if ( !read_check(sock, addr->addr, addr->addr_size, "read domain") ) {
                free(addr->addr);
                free(addr);
                return NULL;
            }
            break;
        case 4:
            addr->addr = malloc(16);
            if ( !read_check(sock, addr->addr, 16, "read ipv6") ) {
                free(addr->addr);
                free(addr);
                return NULL;
            }
            break;
        default:
            return NULL;
    }
    if ( !read_check(sock, &addr->port, 2, "read port") ) {
        free(addr->addr);
        free(addr);
        return NULL;
    }
    return addr;
}

bool send_socks5_response(int sock, uint8_t status, socks5_address *address) {
    uint8_t response[3] = {0x05, status, 0x00};
    int n;

    if ( !write_check(sock, response, 3, "write response") ) return false;

    switch (address->atype) {
        case 1:
            if ( !write_check(sock, &address->atype, 1, "write atype") ) return false;
            if ( !write_check(sock, address->addr, 4, "write addr") ) return false;
            break;
        case 3:
            if ( !write_check(sock, &address->atype, 1, "write atype") ) return false;
            if ( !write_check(sock, &address->addr_size, 1, "write addr_size") ) return false;
            if ( !write_check(sock, address->addr, address->addr_size, "write addr") ) return false;
            break;
        case 4:
            if ( !write_check(sock, &address->atype, 1, "write atype") ) return false;
            if ( !write_check(sock, address->addr, 16, "write addr") ) return false;
            break;
        default:
            return false;
    }

    if ( !write_check(sock, &address->port, 2, "write port") ) return false;
    return true;    
}

bool handle_connect_command(int sock) {
    socks5_address *addr = read_socks5_address(sock);
    int new_sock;
    if (addr == NULL) return false;

    // If address is IPv4
    if (addr->atype == 1) {
        new_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (new_sock < 0) {
            perror("socket");
            return false;
        }

        // Connect forward
        struct sockaddr_in forward_addr;
        forward_addr.sin_family = AF_INET;
        forward_addr.sin_port = addr->port;
        forward_addr.sin_addr.s_addr = *((uint32_t *)addr->addr);
        int n = connect(new_sock, (struct sockaddr *)&forward_addr, sizeof(forward_addr));
        if (n < 0) {
            perror("connect");
            return false;
        }

        // Send success response
        if ( !send_socks5_response(sock, 0x00, addr) ) return false;

        return connect_sockets(sock, new_sock);
    } else if ( addr->atype == 3) {
        // Resolve hostname to IP address in network byte order
        struct hostent *host = gethostbyname((char *)addr->addr);
        if (host == NULL) {
            perror("gethostbyname");
            return false;
        } else if (host->h_addrtype != AF_INET) {
            perror("gethostbyname");
            return false;
        } else if (host->h_addr_list[0] == NULL) {
            perror("gethostbyname");
            return false;
        } else {
            new_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (new_sock < 0) {
                perror("socket");
                return false;
            }
            // Connect forward
            struct sockaddr_in forward_addr;
            forward_addr.sin_family = AF_INET;
            forward_addr.sin_port = addr->port;
            forward_addr.sin_addr.s_addr = *((uint32_t *)host->h_addr_list[0]);
            int n = connect(new_sock, (struct sockaddr *)&forward_addr, sizeof(forward_addr));
            if (n < 0) {
                perror("connect");
                return false;
            }
            // Send success response
            if ( !send_socks5_response(sock, 0x00, addr) ) return false;
            return connect_sockets(sock, new_sock);
        }
    }

    else send_socks5_response(sock, 0x08, addr);

}

bool handle_new_socks5_connection(int sockfd) {
    uint8_t version, n, command, reserved;
    uint8_t *methods = get_requested_auth_methods(sockfd);
    if (methods == NULL) {
        return false;
    }

    if (!authenticate_user(sockfd, methods)) {
        return false;
    }

    free(methods);

    if (!read_check(sockfd, &version, 1, "read version")) return false;

    if (version != 0x05) {
        printf("Invalid version: %d\n", version);
        return false;
    }

    if (!read_check(sockfd, &command, 1, "read command")) return false;
    if (!read_check(sockfd, &reserved, 1, "read reserved")) return false;

    if (reserved != 0x00) {
        printf("Invalid reserved: %d\n", reserved);
        return false;
    }

    if (command == 0x01) {
        printf("Connect command\n");
        return handle_connect_command(sockfd);
    } else if (command == 0x02) {
        printf("Bind command\n");
        //return handle_bind_command(sockfd);
    } else if (command == 0x03) {
        printf("UDP associate command\n");
        //return handle_udp_associate_command(sockfd);
    } else {
        printf("Invalid command: %d\n", command);
        return false;
    }

    return true;
}