#include <telnet_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <uthash.h>
#include <generics.h>

static int _telnetSocket = -1;
static char _telnetHost[256] = "127.0.0.1";  /* Default host, inherited from -s option */
static int _telnetPort = 4444;               /* Default port, configured via -W option */

struct memCache {
    uint32_t addr;
    uint32_t value;
    uint64_t timestamp;
    UT_hash_handle hh;
};
static struct memCache *_memCache = NULL;

void telnet_set_connection_params(const char *host, int port)
{
    if (host && strlen(host) > 0)
    {
        strncpy(_telnetHost, host, sizeof(_telnetHost) - 1);
        _telnetHost[sizeof(_telnetHost) - 1] = '\0';
    }
    if (port > 0)
    {
        _telnetPort = port;
    }
}

static void _drainSocket(int socket) 
{
    char drain[256];
    struct timeval tv = { 0, 10000 };
    fd_set readfds;
    
    FD_ZERO(&readfds);
    FD_SET(socket, &readfds);
    
    while (select(socket + 1, &readfds, NULL, NULL, &tv) > 0) 
    {
        if (recv(socket, drain, sizeof(drain), MSG_DONTWAIT) <= 0)
            break;
        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
    }
}

static int _readLine(int socket, char *buffer, int maxlen, int timeout_ms) 
{
    int pos = 0;
    fd_set readfds;
    struct timeval timeout;
    
    while (pos < maxlen - 1) 
    {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        
        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        
        if (select(socket + 1, &readfds, NULL, NULL, &timeout) <= 0)
            break;
        
        char c;
        int len = recv(socket, &c, 1, 0);
        if (len <= 0)
            break;
        
        if (c == '\0')
            continue;
        if ((unsigned char)c == 0xFF) 
        {
            recv(socket, &c, 1, 0);
            recv(socket, &c, 1, 0);
            continue;
        }
        
        buffer[pos++] = c;
        
        if (c == '\n') 
        {
            if (pos > 1 && buffer[pos-2] == '\r') 
            {
                buffer[pos-2] = '\n';
                pos--;
            }
            buffer[pos] = '\0';
            return pos;
        }
    }
    
    buffer[pos] = '\0';
    return pos;
}

int telnet_connect(int port)
{
    if (_telnetSocket >= 0)
        return _telnetSocket;

    _telnetSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (_telnetSocket < 0)
    {
        genericsReport(V_DEBUG, "Failed to create socket\n");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* Resolve hostname using getaddrinfo to support both IPs and hostnames */
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(_telnetHost, port_str, &hints, &result) == 0 && result)
    {
        memcpy(&addr, result->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(result);
    }
    else
    {
        /* Fallback to inet_addr for direct IP addresses */
        addr.sin_addr.s_addr = inet_addr(_telnetHost);
        if (addr.sin_addr.s_addr == INADDR_NONE)
        {
            genericsReport(V_DEBUG, "Failed to resolve hostname %s\n", _telnetHost);
            close(_telnetSocket);
            _telnetSocket = -1;
            return -1;
        }
    }

    if (connect(_telnetSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        genericsReport(V_DEBUG, "Failed to connect to %s:%d\n", _telnetHost, port);
        close(_telnetSocket);
        _telnetSocket = -1;
        return -1;
    }
    
    char buffer[1024];
    recv(_telnetSocket, buffer, sizeof(buffer), MSG_DONTWAIT);
    
    unsigned char no_echo[] = { 255, 251, 1 };
    send(_telnetSocket, no_echo, sizeof(no_echo), 0);
    
    return _telnetSocket;
}

void telnet_disconnect(void) 
{
    if (_telnetSocket >= 0) 
    {
        close(_telnetSocket);
        _telnetSocket = -1;
    }
    
    struct memCache *cached, *tmp;
    HASH_ITER(hh, _memCache, cached, tmp) 
    {
        HASH_DEL(_memCache, cached);
        free(cached);
    }
    _memCache = NULL;
}

bool telnet_is_connected(void) 
{
    return _telnetSocket >= 0;
}

uint32_t telnet_read_memory_word(uint32_t address)
{
    struct memCache *cached;
    HASH_FIND_INT(_memCache, &address, cached);
    if (cached) {
        return cached->value;
    }

    if (telnet_connect(_telnetPort) < 0)
        return 0;
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "mdw 0x%08x 1\n", address);
    
    if (send(_telnetSocket, cmd, strlen(cmd), 0) < 0) {
        fprintf(stderr, "Failed to send telnet command\n");
        telnet_disconnect();
        return 0;
    }
    
    char line[256];
    uint32_t value = 0;
    int found = 0;
    int lines_read = 0;
    
    while (lines_read < 5) {
        int len = _readLine(_telnetSocket, line, sizeof(line), 500);
        if (len <= 0)
            break;
        
        lines_read++;
        
        if (strstr(line, "mdw"))
            continue;

        char *hex_ptr = strstr(line, "0x");
        if (hex_ptr) {
            uint32_t addr, val;
            if (sscanf(hex_ptr, "0x%x: %x", &addr, &val) == 2) {
                if (addr == address) {
                    value = val;
                    found = 1;
                    break;
                }
            }
        }

        if (strstr(line, "> ") && !hex_ptr)
            break;
    }
    
    if (found) {
        cached = malloc(sizeof(struct memCache));
        if (cached) {
            cached->addr = address;
            cached->value = value;
            cached->timestamp = genericsTimestampuS();
            HASH_ADD_INT(_memCache, addr, cached);
        }
    }
    
    return value;
}

char* telnet_read_memory_string(uint32_t address, char *buffer, size_t maxlen) {
    if (!address || !buffer || maxlen < 2)
        return NULL;

    if (telnet_connect(_telnetPort) < 0)
        return NULL;
    
    char cmd[64];
    int bytes_to_read = 60;
    snprintf(cmd, sizeof(cmd), "mdb 0x%08x %d\n", address, bytes_to_read);
    
    if (send(_telnetSocket, cmd, strlen(cmd), 0) < 0) {
        telnet_disconnect();
        return NULL;
    }
    
    char line[256];
    int found = 0;
    int lines_read = 0;
    size_t out = 0;
    
    while (lines_read < 5) {
        int len = _readLine(_telnetSocket, line, sizeof(line), 500);
        if (len <= 0)
            break;
        
        lines_read++;
        
        if (strstr(line, "mdb"))
            continue;
        
        uint32_t addr;
        char *ptr = strstr(line, "0x");
        if (ptr && sscanf(ptr, "0x%x:", &addr) == 1 && addr == address) {
            char *colon = strchr(ptr, ':');
            if (colon) {
                colon++;
                
                while (*colon && out < maxlen - 1) {
                    while (*colon == ' ' || *colon == '\t')
                        colon++;
                    
                    if (!*colon || *colon == '\n' || *colon == '\r')
                        break;
                    
                    unsigned int byte;
                    if (sscanf(colon, "%02x", &byte) != 1)
                        break;
                    
                    buffer[out++] = (char)byte;
                    
                    if (byte == 0)
                        break;
                    
                    colon += 2;
                }
                found = 1;
                break;
            }
        }
        
        if (strstr(line, "> "))
            break;
    }
    
    if (found && out > 0) {
        buffer[out] = '\0';
        return buffer;
    }
    
    return NULL;
}

void telnet_clear_cache_for_tcb(uint32_t tcb_addr) {
    if (!tcb_addr) return;
    
    struct memCache *cached, *tmp;
    HASH_ITER(hh, _memCache, cached, tmp) 
    {
        if (cached->addr >= tcb_addr && cached->addr < (tcb_addr + 256)) {
            HASH_DEL(_memCache, cached);
            free(cached);
        }
    }
}

void telnet_configure_dwt(uint32_t watch_address) {
    if (telnet_connect(_telnetPort) < 0) {
        genericsReport(V_ERROR, "Cannot connect to OpenOCD telnet at %s:%d\n", _telnetHost, _telnetPort);
        return;
    }
    
    char cmd[256];
    char response[1024];
    
    snprintf(cmd, sizeof(cmd), "rtos_dwt_config 0x%08X\n", watch_address);
    send(_telnetSocket, cmd, strlen(cmd), 0);
    _readLine(_telnetSocket, response, sizeof(response), 500);
    _drainSocket(_telnetSocket);

    genericsReport(V_INFO, "Configured DWT comparator 1 to watch 0x%08X\n", watch_address);
}

void telnet_configure_dwt2(uint32_t watch_address) {
    if (telnet_connect(_telnetPort) < 0) {
        genericsReport(V_ERROR, "Cannot connect to OpenOCD telnet at %s:%d for DWT2\n", _telnetHost, _telnetPort);
        return;
    }

    char cmd[256];
    char response[1024];

    snprintf(cmd, sizeof(cmd), "rtos_dwt2_config 0x%08X\n", watch_address);
    send(_telnetSocket, cmd, strlen(cmd), 0);
    _readLine(_telnetSocket, response, sizeof(response), 500);
    _drainSocket(_telnetSocket);

    genericsReport(V_INFO, "Configured DWT comparator 2 to watch 0x%08X (data write with value)\n", watch_address);
}

void telnet_configure_exception_trace(bool enable) {
    /* Always use a fresh connection for exception trace config */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        genericsReport(V_ERROR, "Failed to create socket for exception trace\n");
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(_telnetPort);

    /* Resolve hostname using getaddrinfo to support both IPs and hostnames */
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", _telnetPort);

    if (getaddrinfo(_telnetHost, port_str, &hints, &result) == 0 && result)
    {
        memcpy(&server_addr, result->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(result);
    }
    else
    {
        /* Fallback to inet_addr for direct IP addresses */
        server_addr.sin_addr.s_addr = inet_addr(_telnetHost);
        if (server_addr.sin_addr.s_addr == INADDR_NONE)
        {
            genericsReport(V_ERROR, "Failed to resolve hostname %s for exception trace\n", _telnetHost);
            close(sock);
            return;
        }
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        genericsReport(V_ERROR, "Cannot connect to %s:%d for exception trace\n", _telnetHost, _telnetPort);
        close(sock);
        return;
    }
    
    /* Wait for and clear the OpenOCD banner */
    char response[1024];
    _readLine(sock, response, sizeof(response), 500);
    
    char cmd[256];
    if (enable) {
        snprintf(cmd, sizeof(cmd), "exception_trace_enable\n");
        genericsReport(V_INFO, "TELNET: Sending command: exception_trace_enable\n");
    } else {
        snprintf(cmd, sizeof(cmd), "exception_trace_disable\n");
        genericsReport(V_INFO, "TELNET: Sending command: exception_trace_disable\n");
    }
    
    int sent = send(sock, cmd, strlen(cmd), 0);
    if (sent > 0) {
        genericsReport(V_INFO, "TELNET: Sent exception trace command (%d bytes)\n", sent);
        _readLine(sock, response, sizeof(response), 500);
        genericsReport(V_INFO, "TELNET: Response: %s", response);
    }
    
    close(sock);
}