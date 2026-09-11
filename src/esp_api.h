#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wesp {

static_assert(sizeof(void*) == 8, "This POC requires x64 or ARM64.");
constexpr std::uint32_t kProcessCreate = 1000;
constexpr std::uint32_t kProcessCommandLine = 1;
constexpr std::uint32_t kProcessId = 6;
constexpr std::uint32_t kProcessImagePath = 20;

struct ClientDescriptor {
    GUID id{};
    const wchar_t* name = nullptr;
    const wchar_t* altitude = nullptr;
};
static_assert(sizeof(ClientDescriptor) == 0x20);

// Recovered from espclient.dll 0.1.0.154553750 (ARM64); not a public SDK.
// These are DLL-facing descriptors, not driver wire messages.
struct EventQueueDescriptor {
    GUID id{};
    std::uint32_t version = 1;
    std::uint32_t type = 1;
    std::uint32_t capacity = 64 * 1024 * 1024;
    std::uint32_t format = 1;
};

// Only the ProcessCreate fields used by this POC are named. Reserved bytes keep
// the descriptor ABI identical to the recovered 0x530-byte external layout.
struct alignas(8) ProcessCreateConfig {
    std::uint32_t mask = 0x08;
    std::array<std::uint8_t, 0x2c> reserved_04{};
    std::uint32_t property_count = 0;
    std::uint32_t reserved_34 = 0;
    const std::uint32_t* property_ids = nullptr;
    std::array<std::uint8_t, 0x4f0> reserved_40{};
};

struct RuleDescriptor {
    GUID id{};
    std::uint64_t order_group = 100;
    std::uint32_t lifetime = 1;
    std::array<std::uint8_t, 4> reserved_1c{};
    std::uint32_t event_type = kProcessCreate;
    std::array<std::uint8_t, 0x3c> reserved_24{};
    const ProcessCreateConfig* event_config = nullptr;
    std::array<std::uint8_t, 0x3d8> reserved_68{};
    std::uint32_t modification = 0;
    std::array<std::uint8_t, 0x0c> reserved_444{};
    std::uint32_t action = 1;
    std::uint32_t reserved_454 = 0;
    void* action_object = nullptr;
    std::uint64_t reserved_460 = 0;
};

struct RuleUpdate {
    std::uint32_t operation = 1;
    std::uint32_t reserved_04 = 0;
    void* rule = nullptr;
    std::uint64_t reserved_10 = 0;
};

static_assert(sizeof(EventQueueDescriptor) == 0x20);
static_assert(sizeof(ProcessCreateConfig) == 0x530);
static_assert(alignof(ProcessCreateConfig) == 8);
static_assert(offsetof(ProcessCreateConfig, property_count) == 0x30);
static_assert(offsetof(ProcessCreateConfig, property_ids) == 0x38);
static_assert(sizeof(RuleDescriptor) == 0x468);
static_assert(offsetof(RuleDescriptor, event_config) == 0x60);
static_assert(offsetof(RuleDescriptor, modification) == 0x440);
static_assert(offsetof(RuleDescriptor, action) == 0x450);
static_assert(offsetof(RuleDescriptor, action_object) == 0x458);
static_assert(sizeof(RuleUpdate) == 0x18);
static_assert(offsetof(RuleUpdate, rule) == 0x08);

struct ProcessCreateRule {
    std::array<std::uint32_t, 3> properties{
        kProcessId,
        kProcessImagePath,
        kProcessCommandLine,
    };
    ProcessCreateConfig config{};
    RuleDescriptor descriptor;

    ProcessCreateRule(const GUID& rule_id, void* queue) {
        config.property_count = static_cast<std::uint32_t>(properties.size());
        config.property_ids = properties.data();
        descriptor.id = rule_id;
        descriptor.event_config = &config;
        descriptor.action_object = queue;
    }

    ProcessCreateRule(const ProcessCreateRule&) = delete;
    ProcessCreateRule& operator=(const ProcessCreateRule&) = delete;
};

// Normalized notification prefix, after espclient has fixed relative pointers.
// The remaining client state stays opaque; only the payload bounds are used.
struct EventDataPrefix {
    std::uint64_t instance_id;
    GUID rule_id;
    std::array<std::uint8_t, 0x60> reserved_18;
    std::uint32_t event_type;
    std::uint32_t reserved_7c;
};

struct NotificationHeader {
    GUID queue_id;
    std::uint32_t reserved_10;
    std::uint32_t reserved_14;
    const EventDataPrefix* data;
    std::array<std::uint8_t, 0x28> reserved_20;
    std::uint32_t external_size;
    std::uint32_t reserved_4c;
    void* owner;
    const void* external_payload;
    std::array<std::uint8_t, 0x10> reserved_60;
};

struct Property {
    std::uint32_t id;
    std::uint32_t type;
    std::uint64_t value;
};

static_assert(sizeof(EventDataPrefix) == 0x80);
static_assert(offsetof(EventDataPrefix, event_type) == 0x78);
static_assert(sizeof(NotificationHeader) == 0x70);
static_assert(offsetof(NotificationHeader, data) == 0x18);
static_assert(offsetof(NotificationHeader, external_size) == 0x48);
static_assert(offsetof(NotificationHeader, external_payload) == 0x58);
static_assert(sizeof(Property) == 0x10);

} // namespace wesp
