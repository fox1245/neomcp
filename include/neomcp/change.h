// Changes of what a server offers, and how the sessions that serve its clients hear of them.
//
// A host (see tool_host.h) says which kinds of change it reports (ChangeSupport) and lets a session register a
// listener for them (ToolHost::watch). A session declares the matching capabilities (`listChanged` for a list,
// `subscribe` for resources) and tells its client of each change the way the revision's `change_feed` mechanism names:
// as notifications of the connection a handshake opened, or on the subscriptions the client opened.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace neomcp {

enum class ChangeKind {
    tools_list,       // the tools a host lists
    prompts_list,     // the prompts it lists
    resources_list,   // the resources or the resource templates it lists
    resource_updated  // the content of one resource (Change::uri)
};

struct Change {
    ChangeKind kind = ChangeKind::tools_list;
    std::string uri;  // resource_updated: the resource whose content changed; empty otherwise
};

// The kinds of change a host reports. A session declares the matching capability for each one the host has things
// of: `listChanged` of tools, prompts and resources, and `subscribe` of resources for their updates.
struct ChangeSupport {
    bool tools_list = false;
    bool prompts_list = false;
    bool resources_list = false;
    bool resource_updates = false;
};

// What a client asks to be told: the lists whose changes it wants, and the resources whose updates it wants.
struct ChangeFilter {
    bool tools_list = false;
    bool prompts_list = false;
    bool resources_list = false;
    std::vector<std::string> resources;  // distinct, in the order given

    // Whether the change is one the filter asks for.
    bool wants(const Change& change) const;
};

// Hears of a host's changes while it is registered (ToolHost::watch).
class ChangeListener {
public:
    virtual ~ChangeListener() = default;
    // One change. Called with no lock of the host held, on a thread of the host's choosing (possibly on several at
    // once), and possibly shortly after the registration was reset: a listener ignores what it no longer wants. Must
    // not block for long and must not throw.
    virtual void changed(const Change& change) = 0;
    // The host reports no more changes (it stops): called once, after the last changed() the host started, and the
    // registration is gone. Must not throw.
    virtual void ended() = 0;
};

// One registration of a listener with a host. Resetting it, or destroying it, ends the registration: the host holds the
// listener no more. Move-only; an empty registration (the host reports no changes) does nothing.
class ChangeRegistration {
public:
    ChangeRegistration() = default;
    // `release` ends the registration; it is called at most once, and must not throw.
    explicit ChangeRegistration(std::function<void()> release) : release_(std::move(release)) {}
    ChangeRegistration(ChangeRegistration&& other) noexcept : release_(std::exchange(other.release_, nullptr)) {}
    ChangeRegistration& operator=(ChangeRegistration&& other) noexcept;
    ChangeRegistration(const ChangeRegistration&) = delete;
    ChangeRegistration& operator=(const ChangeRegistration&) = delete;
    ~ChangeRegistration() { reset(); }

    void reset() noexcept;
    explicit operator bool() const noexcept { return static_cast<bool>(release_); }

private:
    std::function<void()> release_;
};

}  // namespace neomcp
