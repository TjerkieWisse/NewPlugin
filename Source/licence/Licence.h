#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

// Licence keys, sold through Polar (polar.sh). A key works on two computers: activating takes one
// of its two places, deactivating gives it back (in KITNAP, or in the buyer's Polar account). KITNAP
// checks the key with Polar about once a day while online, and keeps working offline for a month
// after the last check. Polar's licence calls need no secret, so nothing in the plugin has to be kept
// hidden: the organization ID only says which store the key belongs to.
namespace kitnap
{
// The store details, set when building (CMake: KITNAP_POLAR_ORGANIZATION_ID and friends, see the
// README). Without an organization ID there is no licence check at all.
struct LicenceConfig
{
    juce::String organizationId;
    juce::String benefitId;  // optional: only keys of this licence key benefit count
    juce::String apiBase;    // https://api.polar.sh, or https://sandbox-api.polar.sh to try things out
    juce::String buyUrl;     // where "Buy KITNAP" goes
    juce::String accountUrl; // the buyer's Polar customer portal: their key, and their computers

    bool enabled() const { return organizationId.isNotEmpty(); }
    static LicenceConfig fromBuild();
};

// One POST with a JSON body. Status 0: no answer at all (offline, timed out, cancelled).
struct HttpReply
{
    int status = 0;
    juce::var json;
};

class HttpClient
{
public:
    virtual ~HttpClient() = default;
    virtual HttpReply post (const juce::String& url, const juce::var& body) = 0;
    virtual void cancel() {} // from another thread: stop the current request and any later ones
};

// The real one: HTTPS through the operating system (curl on Linux).
class WebHttpClient : public HttpClient
{
public:
    HttpReply post (const juce::String& url, const juce::var& body) override;
    void cancel() override;

private:
    juce::CriticalSection lock;
    juce::WebInputStream* current = nullptr;
    bool cancelled = false;
};

// What is kept on this computer about its activation.
struct LicenceRecord
{
    juce::String key, displayKey, activationId, label;
    juce::Time checked;  // when Polar last said the key is good
    juce::Time lastSeen; // the latest time KITNAP ran with it: a clock set back shows up against this

    bool valid() const { return key.isNotEmpty() && activationId.isNotEmpty(); }
};

enum class LicenceState
{
    off,        // this build has no licence check
    none,       // not activated on this computer
    active,
    needsCheck, // activated, but not checked for too long (or the clock went back): go online once
};

// The licence logic, without threads: every call that talks to Polar blocks until it has an answer.
// LicenceService runs it in the background for the plugin; the test drives it directly.
class Licence
{
public:
    static constexpr int checkAfterHours = 24;
    static constexpr int offlineDays = 30;

    Licence (LicenceConfig, juce::File storage, juce::String deviceId, juce::String computerName, HttpClient&);

    const LicenceConfig& config() const { return cfg; }
    const LicenceRecord& record() const { return rec; }
    LicenceState state (juce::Time now) const;
    static LicenceState stateOf (const LicenceRecord&, bool enabled, juce::Time now);
    bool dueForCheck (juce::Time now) const;

    // Each returns an empty string when it worked, or what to tell the person.
    juce::String activate (const juce::String& key, juce::Time now);
    juce::String deactivate();

    enum class Check { good, removed, noAnswer };
    Check check (juce::Time now, juce::String& message);

    // Notes that KITNAP ran now (see LicenceRecord::lastSeen).
    void seen (juce::Time now);

    // The machine-specific value sent along with every activation, so it only validates here.
    juce::String deviceTag() const;

private:
    juce::var request (std::initializer_list<std::pair<const char*, juce::var>> fields) const;
    juce::String url (const char* action) const;
    juce::String signature (const LicenceRecord&) const;
    void load();
    void save();
    void forget();

    LicenceConfig cfg;
    juce::File file;
    juce::String device, computer;
    HttpClient& http;
    LicenceRecord rec;
};

// The licence for the whole plugin process (all KITNAP instances share it through
// juce::SharedResourcePointer). Activating, deactivating and checking happen on its own thread; it
// sends a change message whenever something the panel shows has changed.
class LicenceService : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    LicenceService();
    ~LicenceService() override;

    const LicenceConfig& config() const { return licence.config(); }
    bool enabled() const { return config().enabled(); }
    LicenceState state() const;
    bool canMakeKits() const;
    LicenceRecord record() const;
    bool isBusy() const;
    juce::String message() const;   // the outcome of the last activate/deactivate/check
    bool lastActionFailed() const;

    void activate (const juce::String& key);
    void deactivate();
    void checkNow();

   #if KITNAP_LICENCE_PREVIEW
    // For the screenshots only: pretend a key was activated, without asking anyone.
    void previewActivate (const juce::String& displayKey, const juce::String& label);
   #endif

private:
    void run() override;
    void enqueue (std::function<void()> action);
    void publish (const juce::String& message, bool failed);

    WebHttpClient http;
    Licence licence;

    juce::CriticalSection lock;
    LicenceRecord shown;
    juce::String lastMessage;
    bool failed = false;
    std::vector<std::function<void()>> queue;
    bool busy = false;
};
} // namespace kitnap
