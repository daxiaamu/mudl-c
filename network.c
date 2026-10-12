#include "network.h"

#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int bind_address(SOCKET fd, const struct sockaddr* addr, int addr_len,
                        const char* interface_name, char* err, int err_n) {
    if (bind(fd, addr, addr_len) == 0)
        return 0;

    _snprintf(err, err_n, "Cannot bind interface %s (WinSock %d)",
              interface_name, WSAGetLastError());
    return -1;
}

static bool adapter_name_matches(const IP_ADAPTER_ADDRESSES* adapter,
                                 const char* name, const wchar_t* wide_name) {
    if (adapter->AdapterName && _stricmp(adapter->AdapterName, name) == 0)
        return true;
    if (adapter->FriendlyName && _wcsicmp(adapter->FriendlyName, wide_name) == 0)
        return true;
    if (adapter->Description && _wcsicmp(adapter->Description, wide_name) == 0)
        return true;
    return false;
}

static bool address_matches(const struct sockaddr* left,
                            const struct sockaddr* right) {
    if (!left || !right || left->sa_family != right->sa_family)
        return false;
    if (left->sa_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)left;
        const struct sockaddr_in* b = (const struct sockaddr_in*)right;
        return a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (left->sa_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)left;
        const struct sockaddr_in6* b = (const struct sockaddr_in6*)right;
        return memcmp(&a->sin6_addr, &b->sin6_addr,
                      sizeof(a->sin6_addr)) == 0;
    }
    return false;
}

static int set_outgoing_interface(SOCKET fd, int family,
                                  const IP_ADAPTER_ADDRESSES* adapter,
                                  const char* interface_name,
                                  char* err, int err_n) {
    DWORD index;
    int level;
    int option;
    if (family == AF_INET) {
        index = htonl(adapter->IfIndex);
        level = IPPROTO_IP;
        option = IP_UNICAST_IF;
    } else {
        index = adapter->Ipv6IfIndex;
        level = IPPROTO_IPV6;
        option = IPV6_UNICAST_IF;
    }

    if (setsockopt(fd, level, option, (const char*)&index, sizeof(index)) == 0)
        return 0;

    _snprintf(err, err_n,
              "Cannot select outgoing interface %s (WinSock %d)",
              interface_name, WSAGetLastError());
    return -1;
}

int network_bind_interface(SOCKET fd, int family, const char* interface_name,
                           char* err, int err_n) {
    if (!interface_name || !interface_name[0])
        return 0;

    struct sockaddr_storage requested;
    memset(&requested, 0, sizeof(requested));
    bool numeric_address = false;
    if (family == AF_INET) {
        struct sockaddr_in* local = (struct sockaddr_in*)&requested;
        local->sin_family = AF_INET;
        numeric_address = InetPtonA(AF_INET, interface_name,
                                    &local->sin_addr) == 1;
    } else if (family == AF_INET6) {
        struct sockaddr_in6* local = (struct sockaddr_in6*)&requested;
        local->sin6_family = AF_INET6;
        numeric_address = InetPtonA(AF_INET6, interface_name,
                                    &local->sin6_addr) == 1;
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       interface_name, -1, NULL, 0);
    if (wide_len <= 0) {
        _snprintf(err, err_n, "Invalid UTF-8 interface name: %s", interface_name);
        return -1;
    }

    wchar_t* wide_name = (wchar_t*)calloc((size_t)wide_len, sizeof(wchar_t));
    if (!wide_name) {
        _snprintf(err, err_n, "Out of memory while resolving interface");
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, interface_name, -1,
                        wide_name, wide_len);

    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES* adapters = NULL;
    ULONG status;
    do {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (!adapters) {
            free(wide_name);
            _snprintf(err, err_n, "Out of memory while resolving interface");
            return -1;
        }
        status = GetAdaptersAddresses((ULONG)family,
                                      GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, adapters, &size);
    } while (status == ERROR_BUFFER_OVERFLOW);

    if (status != NO_ERROR) {
        free(adapters);
        free(wide_name);
        _snprintf(err, err_n, "Cannot enumerate network interfaces (error %lu)",
                  (unsigned long)status);
        return -1;
    }

    int result = -1;
    bool matched = false;
    bool address_found = false;
    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter;
         adapter = adapter->Next) {
        bool adapter_matches = !numeric_address &&
            adapter_name_matches(adapter, interface_name, wide_name);
        if (numeric_address) {
            for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
                 unicast; unicast = unicast->Next) {
                if (address_matches(unicast->Address.lpSockaddr,
                                    (const struct sockaddr*)&requested)) {
                    adapter_matches = true;
                    break;
                }
            }
        }
        if (!adapter_matches)
            continue;
        matched = true;

        if (set_outgoing_interface(fd, family, adapter, interface_name,
                                   err, err_n) != 0)
            goto done;

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr ||
                unicast->Address.lpSockaddr->sa_family != family)
                continue;
            if (numeric_address &&
                !address_matches(unicast->Address.lpSockaddr,
                                 (const struct sockaddr*)&requested))
                continue;
            address_found = true;
            result = bind_address(fd, unicast->Address.lpSockaddr,
                                  unicast->Address.iSockaddrLength,
                                  interface_name, err, err_n);
            if (result == 0)
                goto done;
        }
    }

    if (numeric_address && !matched)
        _snprintf(err, err_n, "Local interface address not found: %s",
                  interface_name);
    else if (matched && !address_found)
        _snprintf(err, err_n, "Interface %s has no compatible local address",
                  interface_name);
    else if (!matched)
        _snprintf(err, err_n, "Network interface not found: %s", interface_name);

done:
    free(adapters);
    free(wide_name);
    return result;
}
