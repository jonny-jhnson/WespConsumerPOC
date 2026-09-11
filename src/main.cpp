#include "esp_api.h"
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kClientName[] = L"WespResearchClient";
constexpr wchar_t kClientAltitude[] = L"385000.12345";
using namespace wesp;

std::mutex g_output_lock;
std::atomic_bool g_stop = false;

void Print(std::wstring_view text) {
    const std::scoped_lock lock(g_output_lock);
    std::wcout << text << std::endl;
}

std::wstring ErrorText(HRESULT result) {
    DWORD code = static_cast<DWORD>(result);
    if (HRESULT_FACILITY(result) == FACILITY_WIN32) code = HRESULT_CODE(result);

    std::array<wchar_t, 512> buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr);

    std::wstring text = length ? std::wstring(buffer.data(), length)
                               : L"Unknown error";
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

void PrintFailure(std::wstring_view function, HRESULT result) {
    std::wostringstream line;
    line << L"[-] " << function << L" failed: 0x" << std::hex
         << std::uppercase << std::setw(8) << std::setfill(L'0')
         << static_cast<std::uint32_t>(result) << L" (" << ErrorText(result)
         << L")";
    Print(line.str());
}

std::wstring GuidToString(const GUID& guid) {
    std::array<wchar_t, 40> text{};
    StringFromGUID2(guid, text.data(), static_cast<int>(text.size()));
    return text.data();
}

bool ParseGuid(std::wstring_view text, GUID& guid) {
    const std::wstring copy(text);
    return !text.empty() && SUCCEEDED(CLSIDFromString(copy.c_str(), &guid)) &&
           !IsEqualGUID(guid, GUID_NULL);
}

bool NewGuid(GUID& guid) {
    const HRESULT result = CoCreateGuid(&guid);
    if (FAILED(result)) PrintFailure(L"CoCreateGuid", result);
    return SUCCEEDED(result);
}

// espclient.dll is present on WESP-enabled builds, but the matching SDK
// headers are not public. These are the exports used by this POC.
struct EspClientApi {
    using RegisterClient = HRESULT(WINAPI*)(const ClientDescriptor*);
    using UnregisterClient = HRESULT(WINAPI*)(const GUID*);
    using EnumerateRegisteredClients = HRESULT(WINAPI*)(std::uint32_t*, GUID**);
    using FreeMemory = void(WINAPI*)(void*);
    using Callback = void(WINAPI*)(void*, void*);
    using ConnectClient = HRESULT(WINAPI*)(const GUID*, void**);
    using DisconnectClient = HRESULT(WINAPI*)(void*);
    using CreateEventQueue = HRESULT(WINAPI*)(void*, const EventQueueDescriptor*, void**);
    using CloseEventQueue = HRESULT(WINAPI*)(void*);
    using ConnectEventQueueWithCallback =
        HRESULT(WINAPI*)(void*, std::uint32_t, Callback, void*);
    using DisconnectEventQueue = HRESULT(WINAPI*)(void*);
    using CreateRule = HRESULT(WINAPI*)(const RuleDescriptor*, void**);
    using CloseRule = HRESULT(WINAPI*)(void*);
    using UpdateRules =
        HRESULT(WINAPI*)(void*, std::uint32_t, std::uint32_t, const RuleUpdate*);
    using AllocateEventNotification = void*(WINAPI*)();
    using ArmEventNotification = HRESULT(WINAPI*)(void*, void*);
    using CompleteEventNotification = HRESULT(WINAPI*)(void*);
    using FreeEventNotification = void(WINAPI*)(void*);

    EspClientApi() = default;
    EspClientApi(const EspClientApi&) = delete;
    EspClientApi& operator=(const EspClientApi&) = delete;

    HMODULE module = nullptr;
    RegisterClient register_client = nullptr;
    UnregisterClient unregister_client = nullptr;
    EnumerateRegisteredClients enumerate_clients = nullptr;
    FreeMemory free_memory = nullptr;
    ConnectClient connect_client = nullptr;
    DisconnectClient disconnect_client = nullptr;
    CreateEventQueue create_event_queue = nullptr;
    CloseEventQueue close_event_queue = nullptr;
    ConnectEventQueueWithCallback connect_queue = nullptr;
    DisconnectEventQueue disconnect_queue = nullptr;
    CreateRule create_rule = nullptr;
    CloseRule close_rule = nullptr;
    UpdateRules update_rules = nullptr;
    AllocateEventNotification allocate_notification = nullptr;
    ArmEventNotification arm_notification = nullptr;
    CompleteEventNotification complete_notification = nullptr;
    FreeEventNotification free_notification = nullptr;

    ~EspClientApi() {
        if (module) FreeLibrary(module);
    }

    template <typename T>
    bool Resolve(const char* name, T& function) {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        if (function) return true;
        Print(L"[-] espclient.dll is missing export " +
              std::wstring(name, name + std::strlen(name)));
        return false;
    }

    bool Load() {
        module = LoadLibraryExW(L"espclient.dll",
                                nullptr,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            PrintFailure(L"LoadLibraryExW(espclient.dll)",
                         HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }

        return Resolve("EspRegisterClient", register_client) &&
               Resolve("EspUnregisterClient", unregister_client) &&
               Resolve("EspEnumerateRegisteredClients", enumerate_clients) &&
               Resolve("EspFreeMemory", free_memory) &&
               Resolve("EspConnectClient", connect_client) &&
               Resolve("EspDisconnectClient", disconnect_client) &&
               Resolve("EspCreateEventQueue", create_event_queue) &&
               Resolve("EspCloseEventQueue", close_event_queue) &&
               Resolve("EspConnectEventQueueWithCallback", connect_queue) &&
               Resolve("EspDisconnectEventQueue", disconnect_queue) &&
               Resolve("EspCreateRule", create_rule) &&
               Resolve("EspCloseRule", close_rule) &&
               Resolve("EspUpdateRules", update_rules) &&
               Resolve("EspAllocateEventNotification", allocate_notification) &&
               Resolve("EspArmEventNotification", arm_notification) &&
               Resolve("EspCompleteEventNotification", complete_notification) &&
               Resolve("EspFreeEventNotification", free_notification);
    }
};

bool RegisterClient(const GUID& client_id) {
    EspClientApi api;
    if (!api.Load()) return false;
    const ClientDescriptor descriptor{client_id, kClientName, kClientAltitude};
    const HRESULT result = api.register_client(&descriptor);
    if (FAILED(result)) {
        PrintFailure(L"EspRegisterClient", result);
        return false;
    }
    Print(L"[+] Registered WESP client " + GuidToString(client_id));
    Print(L"    Name     : " + std::wstring(kClientName));
    Print(L"    Altitude : " + std::wstring(kClientAltitude));
    return true;
}

bool EnumerateClients() {
    EspClientApi api;
    if (!api.Load()) return false;
    std::uint32_t count = 0;
    GUID* clients = nullptr;
    const HRESULT result = api.enumerate_clients(&count, &clients);
    if (FAILED(result)) {
        PrintFailure(L"EspEnumerateRegisteredClients", result);
        return false;
    }
    // The DLL owns this allocation; never release it with delete or LocalFree.
    struct ClientList {
        EspClientApi& api;
        GUID* values;
        ~ClientList() { api.free_memory(values); }
    } list{api, clients};
    if (count && !clients) {
        Print(L"[-] espclient.dll returned an invalid client list");
        return false;
    }
    Print(L"[+] Registered WESP clients: " + std::to_wstring(count));
    for (std::uint32_t index = 0; index < count; ++index) {
        Print(L"    " + GuidToString(clients[index]));
    }
    return true;
}

bool RemoveClient(const GUID& client_id) {
    EspClientApi api;
    if (!api.Load()) return false;
    const HRESULT result = api.unregister_client(&client_id);
    if (FAILED(result)) {
        PrintFailure(L"EspUnregisterClient", result);
        return false;
    }
    Print(L"[+] Removed WESP client " + GuidToString(client_id));
    return true;
}

bool Contains(const void* base, std::size_t size,
              const void* address, std::size_t length) {
    if (!base || !address) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    return pointer >= start && length <= size &&
           pointer - start <= size - length;
}

bool IsReadable(const void* address, std::size_t length) {
    if (!address || length == 0) return false;
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (cursor + length < cursor) return false;
    const auto end = cursor + length;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor),
                         &memory,
                         sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        constexpr DWORD readable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((memory.Protect & readable) == 0) return false;

        const auto region_end =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
            memory.RegionSize;
        if (region_end <= cursor) return false;
        cursor = std::min(region_end, end);
    }
    return true;
}

std::wstring ReadStringProperty(std::uint64_t descriptor_address) {
    if (!descriptor_address) return {};
    const auto* descriptor = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(descriptor_address));
    if (!IsReadable(descriptor, 16)) return {};

    std::uint16_t byte_length = 0;
    std::uintptr_t buffer_address = 0;
    std::memcpy(&byte_length, descriptor, sizeof(byte_length));
    std::memcpy(&buffer_address, descriptor + 8, sizeof(buffer_address));
    if (!byte_length || (byte_length & 1) || byte_length > 32766 ||
        !buffer_address ||
        !IsReadable(reinterpret_cast<const void*>(buffer_address),
                    byte_length)) {
        return {};
    }

    return std::wstring(reinterpret_cast<const wchar_t*>(buffer_address),
                        byte_length / sizeof(wchar_t));
}

const Property* FindProcessProperties(
    std::span<const std::uint8_t> notification) {
    // The requested PID, image and command line are returned as three packed
    // {id, type, value} records in the normalized notification.
    constexpr std::size_t property_bytes = sizeof(Property) * 3;
    for (std::size_t offset = 0x78;
         offset + property_bytes <= notification.size();
         offset += 8) {
        std::array<Property, 3> properties{};
        std::memcpy(properties.data(),
                    notification.data() + offset,
                    property_bytes);

        if (properties[0].id == kProcessId &&
            properties[1].id == kProcessImagePath &&
            properties[2].id == kProcessCommandLine &&
            properties[0].type > 0 && properties[0].type <= 32 &&
            properties[1].type > 0 && properties[1].type <= 32 &&
            properties[2].type > 0 && properties[2].type <= 32) {
            return reinterpret_cast<const Property*>(
                notification.data() + offset);
        }
    }
    return nullptr;
}

bool ReadProcessCreate(void* notification, const GUID& queue_id,
                       const GUID& rule_id, EventDataPrefix& event) {
    const auto* raw = static_cast<const std::uint8_t*>(notification) - 8;
    std::uint64_t size = 0;
    std::memcpy(&size, raw, sizeof(size));
    // EspAllocateEventNotification reserves 0x1010 bytes; the normalized
    // allocation begins 16 bytes into it. The DLL owns any external payload.
    if (size < 0x78 || size > 0x1000) return false;

    NotificationHeader header{};
    std::memcpy(&header, notification, sizeof(header));
    if (!IsEqualGUID(header.queue_id, queue_id) ||
        (!Contains(raw, static_cast<std::size_t>(size),
                   header.data, sizeof(event)) &&
         !Contains(header.external_payload, header.external_size,
                   header.data, sizeof(event)))) {
        return false;
    }
    std::memcpy(&event, header.data, sizeof(event));
    return event.event_type == kProcessCreate &&
           IsEqualGUID(event.rule_id, rule_id);
}

struct CallbackState {
    EspClientApi* api = nullptr;
    std::mutex gate;
    void* queue = nullptr;
    GUID queue_id{};
    GUID rule_id{};
    bool stopping = false;
    std::atomic<HRESULT> error{S_OK};
    std::atomic<std::uint64_t> events{0};
};

void PrintProcessCreate(void* notification, CallbackState& state) {
    EventDataPrefix event{};
    std::uint64_t raw_size = 0;
    std::memcpy(&raw_size,
                static_cast<const std::uint8_t*>(notification) - 8,
                sizeof(raw_size));
    if (!ReadProcessCreate(notification, state.queue_id, state.rule_id, event) ||
        raw_size > std::numeric_limits<std::size_t>::max()) {
        state.error.store(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        g_stop.store(true);
        return;
    }
    const auto* raw = static_cast<const std::uint8_t*>(notification) - 8;
    const std::span<const std::uint8_t> bytes(
        raw, static_cast<std::size_t>(raw_size));
    const Property* properties = FindProcessProperties(bytes);

    std::wostringstream output;
    output << L"\n[+] ProcessCreate #" << ++state.events;
    if (properties) {
        const auto pid = static_cast<std::uint32_t>(properties[0].value);
        const std::wstring image = ReadStringProperty(properties[1].value);
        const std::wstring command_line =
            ReadStringProperty(properties[2].value);
        output << L"\n    PID          : "
               << pid
               << L"\n    Image        : "
               << (image.empty() ? L"<unavailable>" : image)
               << L"\n    Command Line : "
               << (command_line.empty() ? L"<unavailable>" : command_line);
    } else {
        output << L"\n    Process properties were not returned";
    }
    Print(output.str());
}

void WINAPI ProcessCreateCallback(void* notification, void* context) noexcept {
    auto& state = *static_cast<CallbackState*>(context);
    // Hold the gate across complete/rearm so shutdown cannot slip between them.
    const std::scoped_lock lock(state.gate);
    if (state.stopping || !notification) return;

    try {
        PrintProcessCreate(notification, state);
    } catch (...) {
        state.error.store(E_FAIL);
        g_stop.store(true);
    }

    // Completion acknowledges the event; rearm releases the old payload and
    // posts the next receive. No pointers into the payload survive this call.
    HRESULT result = state.api->complete_notification(notification);
    if (SUCCEEDED(result) && SUCCEEDED(state.error.load())) {
        result = state.api->arm_notification(state.queue, notification);
    }
    if (FAILED(result)) {
        state.error.store(result);
        g_stop.store(true);
    }
}

BOOL WINAPI ConsoleHandler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT) {
        g_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

struct MonitorSession {
    explicit MonitorSession(EspClientApi& functions) : api(functions) {
        callback.api = &api;
    }
    ~MonitorSession() { Stop(); }

    MonitorSession(const MonitorSession&) = delete;
    MonitorSession& operator=(const MonitorSession&) = delete;

    HRESULT Stop() {
        {
            const std::scoped_lock lock(callback.gate);
            callback.stopping = true;
        }

        HRESULT first_error = S_OK;
        const auto record = [&](std::wstring_view name, HRESULT result) {
            if (FAILED(result)) {
                PrintFailure(name, result);
                if (SUCCEEDED(first_error)) first_error = result;
            }
        };
        if (delivery_connected) {
            const HRESULT result = api.disconnect_queue(queue);
            record(L"EspDisconnectEventQueue", result);
            if (FAILED(result)) {
                // Close also attempts to drain the listener. Never free an
                // active receive or unload its callback code after a failure.
                const HRESULT closed = api.close_event_queue(queue);
                if (FAILED(closed)) {
                    PrintFailure(L"EspCloseEventQueue", closed);
                    std::fputws(L"Cannot drain WESP callbacks; exiting.\n", stderr);
                    std::quick_exit(1);
                }
                queue = nullptr;
            }
            delivery_connected = false;
        }
        if (rule) {
            record(L"EspCloseRule", api.close_rule(rule));
            rule = nullptr;
        }
        if (notification) {
            api.free_notification(notification);
            notification = nullptr;
        }
        if (queue) {
            record(L"EspCloseEventQueue", api.close_event_queue(queue));
            queue = nullptr;
        }
        if (client) {
            record(L"EspDisconnectClient", api.disconnect_client(client));
            client = nullptr;
        }
        return first_error;
    }

    EspClientApi& api;
    CallbackState callback;
    void* client = nullptr;
    void* queue = nullptr;
    void* rule = nullptr;
    void* notification = nullptr;
    bool delivery_connected = false;
};

bool ArmNotification(MonitorSession& session) {
    session.notification = session.api.allocate_notification();
    if (!session.notification) {
        Print(L"[-] EspAllocateEventNotification returned null");
        return false;
    }
    const HRESULT result =
        session.api.arm_notification(session.queue, session.notification);
    if (FAILED(result)) PrintFailure(L"EspArmEventNotification", result);
    return SUCCEEDED(result);
}

bool MonitorProcesses(const GUID& client_id, std::uint32_t seconds) {
    g_stop.store(false, std::memory_order_release);
    EspClientApi api;
    if (!api.Load()) return false;
    MonitorSession session(api);

    // 1. Connect to the registered client. This is the management connection.
    Print(L"[*] Connecting to WESP client " + GuidToString(client_id));
    HRESULT result = api.connect_client(&client_id, &session.client);
    if (FAILED(result)) {
        PrintFailure(L"EspConnectClient", result);
        return false;
    }

    GUID queue_id{};
    GUID rule_id{};
    if (!NewGuid(queue_id) || !NewGuid(rule_id)) return false;

    // 2. Create the queue that the ProcessCreate rule will target.
    EventQueueDescriptor queue_descriptor{queue_id};
    result = api.create_event_queue(
        session.client, &queue_descriptor, &session.queue);
    if (FAILED(result)) {
        PrintFailure(L"EspCreateEventQueue", result);
        return false;
    }

    // 3. Open the delivery connection and post asynchronous receives.
    session.callback.queue = session.queue;
    session.callback.queue_id = queue_id;
    session.callback.rule_id = rule_id;
    result = api.connect_queue(
        session.queue, 1, ProcessCreateCallback, &session.callback);
    if (FAILED(result)) {
        PrintFailure(L"EspConnectEventQueueWithCallback", result);
        return false;
    }
    session.delivery_connected = true;
    if (!ArmNotification(session)) return false;

    // 4. Create a ProcessCreate rule whose action points at the queue.
    ProcessCreateRule process_rule(rule_id, session.queue);
    result = api.create_rule(&process_rule.descriptor, &session.rule);
    if (FAILED(result)) {
        PrintFailure(L"EspCreateRule(ProcessCreate)", result);
        return false;
    }

    // 5. Publish the rule. EspCreateRule only creates the user-mode object.
    RuleUpdate update;
    update.rule = session.rule;
    result = api.update_rules(session.client, 0, 1, &update);
    if (FAILED(result)) {
        PrintFailure(L"EspUpdateRules", result);
        return false;
    }

    Print(L"[+] Event queue created       " + GuidToString(queue_id));
    Print(L"[+] ProcessCreate rule added  " + GuidToString(rule_id));
    if (seconds) {
        Print(L"[*] Monitoring process creation for " +
              std::to_wstring(seconds) + L" seconds. Press Ctrl+C to stop.");
    } else {
        Print(L"[*] Monitoring process creation. Press Ctrl+C to stop.");
    }

    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        PrintFailure(L"SetConsoleCtrlHandler", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    while (!g_stop.load(std::memory_order_acquire) &&
           (!seconds || std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result = session.Stop();
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    const HRESULT callback_error = session.callback.error.load();
    if (FAILED(callback_error)) PrintFailure(L"notification callback", callback_error);

    Print(L"\n[+] Captured " +
          std::to_wstring(session.callback.events.load()) +
          L" process creation event(s)");
    return SUCCEEDED(result) && SUCCEEDED(callback_error);
}

void PrintUsage() {
    std::wcout
        << L"WESP ProcessCreate POC\n\n"
        << L"Usage:\n"
        << L"  wesp-consumer.exe register [client-guid]\n"
        << L"  wesp-consumer.exe clients\n"
        << L"  wesp-consumer.exe monitor <client-guid> [seconds]\n"
        << L"  wesp-consumer.exe remove <client-guid>\n\n"
        << L"Examples:\n"
        << L"  wesp-consumer.exe register\n"
        << L"  wesp-consumer.exe clients\n"
        << L"  wesp-consumer.exe monitor {CLIENT-GUID} 60\n"
        << L"  wesp-consumer.exe remove {CLIENT-GUID}\n";
}

bool ParseSeconds(std::wstring_view text, std::uint32_t& seconds) {
    if (text.empty()) return false;
    std::uint32_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    seconds = value;
    return true;
}

}  // namespace

int Run(int argc, wchar_t** argv) {
    if (argc < 2 || (argc == 2 &&
                     (std::wstring_view(argv[1]) == L"--help" ||
                      std::wstring_view(argv[1]) == L"-h"))) {
        PrintUsage();
        return 0;
    }

    const std::wstring_view command = argv[1];
    if (command == L"register") {
        if (argc > 3) {
            PrintUsage();
            return 2;
        }

        GUID client_id{};
        if (argc == 3) {
            if (!ParseGuid(argv[2], client_id)) {
                Print(L"[-] Invalid client GUID");
                return 2;
            }
        } else if (!NewGuid(client_id)) {
            return 1;
        }
        return RegisterClient(client_id) ? 0 : 1;
    }

    if (command == L"clients") {
        if (argc != 2) {
            PrintUsage();
            return 2;
        }
        return EnumerateClients() ? 0 : 1;
    }

    if (command == L"remove") {
        GUID client_id{};
        if (argc != 3 || !ParseGuid(argv[2], client_id)) {
            PrintUsage();
            return 2;
        }
        return RemoveClient(client_id) ? 0 : 1;
    }

    if (command == L"monitor") {
        GUID client_id{};
        std::uint32_t seconds = 60;
        if ((argc != 3 && argc != 4) || !ParseGuid(argv[2], client_id) ||
            (argc == 4 && !ParseSeconds(argv[3], seconds))) {
            PrintUsage();
            return 2;
        }
        return MonitorProcesses(client_id, seconds) ? 0 : 1;
    }

    PrintUsage();
    return 2;
}

int wmain(int argc, wchar_t** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 1;
    }
}
