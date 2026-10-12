#include "network.h"

#include <iphlpapi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_adapter_name(void) {
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES* adapters = (IP_ADAPTER_ADDRESSES*)malloc(size);
    assert(adapters != NULL);
    ULONG status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                        GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                                        NULL, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES*)malloc(size);
        assert(adapters != NULL);
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, adapters, &size);
    }
    assert(status == NO_ERROR);

    IP_ADAPTER_ADDRESSES* selected = NULL;
    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter;
         adapter = adapter->Next) {
        if (adapter->FriendlyName && adapter->FirstUnicastAddress) {
            selected = adapter;
            break;
        }
    }
    assert(selected != NULL);

    int name_len = WideCharToMultiByte(CP_UTF8, 0, selected->FriendlyName, -1,
                                       NULL, 0, NULL, NULL);
    assert(name_len > 0);
    char* name = (char*)malloc((size_t)name_len);
    assert(name != NULL);
    WideCharToMultiByte(CP_UTF8, 0, selected->FriendlyName, -1,
                        name, name_len, NULL, NULL);

    char err[256] = {0};
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(fd != INVALID_SOCKET);
    assert(network_bind_interface(fd, AF_INET, name, err, sizeof(err)) == 0);
    DWORD interface_index = 0;
    int option_len = sizeof(interface_index);
    assert(getsockopt(fd, IPPROTO_IP, IP_UNICAST_IF,
                      (char*)&interface_index, &option_len) == 0);
    assert(interface_index == selected->IfIndex);
    closesocket(fd);

    free(name);
    free(adapters);
}

int main(void) {
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    char err[256] = {0};
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(fd != INVALID_SOCKET);
    assert(network_bind_interface(fd, AF_INET, "127.0.0.1",
                                  err, sizeof(err)) == 0);

    struct sockaddr_in local;
    int local_len = sizeof(local);
    memset(&local, 0, sizeof(local));
    assert(getsockname(fd, (struct sockaddr*)&local, &local_len) == 0);
    assert(local.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    DWORD interface_index = 0;
    int option_len = sizeof(interface_index);
    assert(getsockopt(fd, IPPROTO_IP, IP_UNICAST_IF,
                      (char*)&interface_index, &option_len) == 0);
    assert(interface_index != 0);
    closesocket(fd);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(fd != INVALID_SOCKET);
    assert(network_bind_interface(fd, AF_INET, "missing-interface-for-test",
                                  err, sizeof(err)) != 0);
    assert(strstr(err, "not found") != NULL);
    closesocket(fd);

    test_adapter_name();

    WSACleanup();
    puts("network interface unit tests passed");
    return 0;
}
