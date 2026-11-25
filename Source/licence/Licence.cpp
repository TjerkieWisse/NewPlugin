#include "Licence.h"
#include "LicenceConfig.h"
#include <juce_cryptography/juce_cryptography.h>

namespace kitnap
{
namespace
{
const char* const noAnswer = "Can't reach the licence server. Check your internet connection and try again.";

juce::String serverProblem (int status)
{
    if (status == 429)
        return "Too many tries in a short time. Wait a minute, then try again.";
    return "The licence server had a problem (" + juce::String (status) + "). Try again in a few minutes.";
}

juce::String detailOf (const juce::var& json)
{
    const auto detail = json.getProperty ("detail", {});
    return detail.isString() ? detail.toString() : juce::String();
}

// Keys get pasted from emails: drop spaces, line breaks and quotes around them.
juce::String cleanKey (const juce::String& key)
{
    return key.removeCharacters (" \t\r\n\"'");
}

// Polar's times have microseconds (2026-11-25T10:00:00.123456Z); JUCE reads up to milliseconds.
juce::Time parseTime (const juce::var& value)
{
    auto text = value.isString() ? value.toString() : juce::String();
    const int dot = text.indexOfChar ('.');
    if (dot > 0)
    {
        int end = dot + 1;
        while (end < text.length() && juce::CharacterFunctions::isDigit (text[end]))
            ++end;
        text = text.substring (0, juce::jmin (end, dot + 4)) + text.substring (end);
    }
    return text.isEmpty() ? juce::Time() : juce::Time::fromISO8601 (text);
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* obj = new juce::DynamicObject();
    for (auto& [name, value] : fields)
        obj->setProperty (name, value);
    return obj;
}
} // namespace

LicenceConfig LicenceConfig::fromBuild()
{
   #if KITNAP_LICENCE_PREVIEW
    return { "00000000-0000-0000-0000-000000000000", {}, {}, "https://polar.sh", "https://polar.sh" };
   #else
    return { KITNAP_POLAR_ORGANIZATION_ID, KITNAP_POLAR_BENEFIT_ID, KITNAP_POLAR_API, KITNAP_BUY_URL, KITNAP_ACCOUNT_URL };
   #endif
}

//==============================================================================
HttpReply WebHttpClient::post (const juce::String& address, const juce::var& body)
{
    juce::WebInputStream stream (juce::URL (address).withPOSTData (juce::JSON::toString (body, true)), true);
    stream.withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json")
          .withConnectionTimeout (15000);
    {
        const juce::ScopedLock sl (lock);
        if (cancelled)
            return {};
        current = &stream;
    }

    // HTTP errors still come with a body (Polar says why); only a failed connection doesn't.
    HttpReply reply;
    if (stream.connect (nullptr))
    {
        reply.status = stream.getStatusCode();
        reply.json = juce::JSON::parse (stream.readEntireStreamAsString());
    }

    const juce::ScopedLock sl (lock);
    current = nullptr;
    if (cancelled)
        return {};
    return reply;
}

void WebHttpClient::cancel()
{
    const juce::ScopedLock sl (lock);
    cancelled = true;
    if (current != nullptr)
        current->cancel();
}

//==============================================================================
Licence::Licence (LicenceConfig c, juce::File storage, juce::String deviceId, juce::String computerName, HttpClient& h)
    : cfg (std::move (c)), file (std::move (storage)), device (std::move (deviceId)), computer (std::move (computerName)), http (h)
{
    load();
}

LicenceState Licence::stateOf (const LicenceRecord& r, bool enabled, juce::Time now)
{
    if (! enabled)
        return LicenceState::off;
    if (! r.valid())
        return LicenceState::none;
    const auto day = juce::RelativeTime::days (1);
    if (now < r.lastSeen - day || now < r.checked - day)
        return LicenceState::needsCheck; // the clock went back
    if (now - r.checked > juce::RelativeTime::days (offlineDays))
        return LicenceState::needsCheck;
    return LicenceState::active;
}

LicenceState Licence::state (juce::Time now) const
{
    return stateOf (rec, cfg.enabled(), now);
}

bool Licence::dueForCheck (juce::Time now) const
{
    const auto s = state (now);
    return s == LicenceState::needsCheck
        || (s == LicenceState::active && now - rec.checked > juce::RelativeTime::hours (checkAfterHours));
}

juce::String Licence::deviceTag() const
{
    return juce::SHA256 (("kitnap-device\n" + device).toUTF8()).toHexString().substring (0, 32);
}

juce::String Licence::url (const char* action) const
{
    return cfg.apiBase.trimCharactersAtEnd ("/") + "/v1/customer-portal/license-keys/" + action;
}

juce::var Licence::request (std::initializer_list<std::pair<const char*, juce::var>> fields) const
{
    auto body = object (fields);
    body.getDynamicObject()->setProperty ("organization_id", cfg.organizationId);
    return body;
}

juce::String Licence::activate (const juce::String& typed, juce::Time now)
{
    const auto key = cleanKey (typed);
    if (key.isEmpty())
        return "Paste your licence key first.";
    if (cfg.apiBase.isEmpty())
        return "This copy of KITNAP can't reach the licence server.";

    // The label is what the buyer sees for this computer in their account, to deactivate it there.
    const auto label = computer.trim().isNotEmpty() ? computer.trim().substring (0, 100) : juce::String ("Computer");
    const auto reply = http.post (url ("activate"),
                                  request ({ { "key", key },
                                             { "label", label },
                                             { "conditions", object ({ { "device", deviceTag() } }) },
                                             { "meta", object ({ { "os", juce::SystemStats::getOperatingSystemName() } }) } }));
    switch (reply.status)
    {
        case 200:
            break;
        case 0:
            return noAnswer;
        case 403:
        {
            const auto detail = detailOf (reply.json);
            if (detail.containsIgnoreCase ("limit"))
                return "This key is already activated on two computers. Deactivate it on one of them first: "
                       "in KITNAP there (Licence, then Deactivate), or in your account.";
            return "This key can't be activated: it was refunded, turned off or has expired."
                 + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String());
        }
        case 404:
            return "That key wasn't found. Copy it again from your purchase email, or from your account.";
        case 422:
            return "That doesn't look like a KITNAP licence key. Copy it again from your purchase email.";
        default:
            return serverProblem (reply.status);
    }

    const auto activationId = reply.json.getProperty ("id", {}).toString();
    const auto licenceKey = reply.json.getProperty ("license_key", {});
    if (activationId.isEmpty() || ! licenceKey.isObject())
        return "The licence server gave an answer KITNAP doesn't understand. Try again in a few minutes.";

    // Make sure it's a key for KITNAP, and a good one; if not, give the place back straight away.
    juce::String problem;
    const auto expires = parseTime (licenceKey.getProperty ("expires_at", {}));
    if (cfg.benefitId.isNotEmpty() && licenceKey.getProperty ("benefit_id", {}).toString() != cfg.benefitId)
        problem = "That's a licence key for another product, not for KITNAP.";
    else if (licenceKey.getProperty ("status", "granted").toString() != "granted")
        problem = "This key can't be activated: it was refunded or turned off.";
    else if (expires.toMilliseconds() > 0 && expires < now)
        problem = "This key has expired.";
    if (problem.isNotEmpty())
    {
        http.post (url ("deactivate"), request ({ { "key", key }, { "activation_id", activationId } }));
        return problem;
    }

    LicenceRecord r;
    r.key = key;
    r.displayKey = licenceKey.getProperty ("display_key", {}).toString();
    if (r.displayKey.isEmpty())
        r.displayKey = "****-" + key.getLastCharacters (6);
    r.activationId = activationId;
    r.label = reply.json.getProperty ("label", label).toString();
    r.checked = r.lastSeen = now;
    rec = r;
    save();
    return {};
}

juce::String Licence::deactivate()
{
    if (! rec.valid())
    {
        forget();
        return {};
    }
    const auto reply = http.post (url ("deactivate"), request ({ { "key", rec.key }, { "activation_id", rec.activationId } }));
    if (reply.status == 204 || reply.status == 200 || reply.status == 404) // 404: it was already gone
    {
        forget();
        return {};
    }
    if (reply.status == 0)
        return "Can't reach the licence server, and deactivating needs it. Check your internet connection and try again.";
    return serverProblem (reply.status);
}

Licence::Check Licence::check (juce::Time now, juce::String& message)
{
    message = {};
    if (! rec.valid() || cfg.apiBase.isEmpty())
        return Check::noAnswer;

    auto body = request ({ { "key", rec.key },
                           { "activation_id", rec.activationId },
                           { "conditions", object ({ { "device", deviceTag() } }) } });
    if (cfg.benefitId.isNotEmpty())
        body.getDynamicObject()->setProperty ("benefit_id", cfg.benefitId);
    const auto reply = http.post (url ("validate"), body);

    if (reply.status == 200)
    {
        const auto expires = parseTime (reply.json.getProperty ("expires_at", {}));
        const bool granted = reply.json.getProperty ("status", "granted").toString() == "granted";
        if (granted && ! (expires.toMilliseconds() > 0 && expires < now))
        {
            rec.checked = rec.lastSeen = now;
            save();
            return Check::good;
        }
    }
    else if (reply.status != 404 && reply.status != 403)
    {
        // No answer, or a hiccup on Polar's side: keep going on the last good check.
        message = reply.status == 0 ? juce::String (noAnswer) : serverProblem (reply.status);
        return Check::noAnswer;
    }

    // Polar no longer knows this activation: deactivated in the account, refunded or turned off.
    forget();
    message = "KITNAP isn't activated on this computer any more: it was deactivated in your account, or the key "
              "was refunded or turned off. Enter your key to activate it again.";
    return Check::removed;
}

void Licence::seen (juce::Time now)
{
    if (rec.valid() && now > rec.lastSeen)
    {
        rec.lastSeen = now;
        save();
    }
}

//==============================================================================
// The file is plain JSON with a signature over its contents, this computer and the store: edited by
// hand, or copied to another computer, it no longer counts.
juce::String Licence::signature (const LicenceRecord& r) const
{
    const auto text = juce::StringArray { "kitnap-licence-1", cfg.organizationId, device, r.key, r.displayKey, r.activationId,
                                          r.label, juce::String (r.checked.toMilliseconds()), juce::String (r.lastSeen.toMilliseconds()) }
                          .joinIntoString ("\n");
    return juce::SHA256 (text.toUTF8()).toHexString();
}

void Licence::load()
{
    rec = {};
    if (! cfg.enabled() || ! file.existsAsFile())
        return;
    const auto json = juce::JSON::parse (file);
    if (! json.isObject() || json.getProperty ("organization", {}).toString() != cfg.organizationId)
        return;

    LicenceRecord r;
    r.key = json.getProperty ("key", {}).toString();
    r.displayKey = json.getProperty ("displayKey", {}).toString();
    r.activationId = json.getProperty ("activation", {}).toString();
    r.label = json.getProperty ("label", {}).toString();
    r.checked = juce::Time ((juce::int64) json.getProperty ("checked", 0));
    r.lastSeen = juce::Time ((juce::int64) json.getProperty ("lastSeen", 0));
    if (json.getProperty ("signature", {}).toString() == signature (r))
        rec = r;
}

void Licence::save()
{
    auto json = object ({ { "organization", cfg.organizationId },
                          { "key", rec.key },
                          { "displayKey", rec.displayKey },
                          { "activation", rec.activationId },
                          { "label", rec.label },
                          { "checked", rec.checked.toMilliseconds() },
                          { "lastSeen", rec.lastSeen.toMilliseconds() },
                          { "signature", signature (rec) } });
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (json));
}

void Licence::forget()
{
    rec = {};
    file.deleteFile();
}

//==============================================================================
namespace
{
juce::File licenceFile()
{
   #if KITNAP_LICENCE_PREVIEW
    auto preview = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KITNAP-licence-preview.json");
    preview.deleteFile();
    return preview;
   #elif JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/FortyVolts/KITNAP/licence.json");
   #elif JUCE_WINDOWS
    // Local, not roaming: an activation belongs to one computer.
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("FortyVolts/KITNAP/licence.json");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("FortyVolts/KITNAP/licence.json");
   #endif
}

juce::String deviceId (const LicenceConfig& config)
{
    if (! config.enabled())
        return {};
    auto id = juce::SystemStats::getUniqueDeviceID();
    if (id.isEmpty())
        id = juce::SystemStats::getComputerName() + "/" + juce::SystemStats::getLogonName();
    return id;
}
} // namespace

LicenceService::LicenceService()
    : juce::Thread ("KITNAP licence"),
      licence (LicenceConfig::fromBuild(), licenceFile(), deviceId (LicenceConfig::fromBuild()),
               juce::SystemStats::getComputerName(), http)
{
    licence.seen (juce::Time::getCurrentTime());
    shown = licence.record();
    if (enabled() && config().apiBase.isNotEmpty())
        startThread (juce::Thread::Priority::low);
}

LicenceService::~LicenceService()
{
    signalThreadShouldExit();
    http.cancel();
    notify();
    stopThread (20000);
}

LicenceState LicenceService::state() const
{
    const juce::ScopedLock sl (lock);
    return Licence::stateOf (shown, enabled(), juce::Time::getCurrentTime());
}

bool LicenceService::canMakeKits() const
{
    const auto s = state();
    return s == LicenceState::off || s == LicenceState::active;
}

LicenceRecord LicenceService::record() const
{
    const juce::ScopedLock sl (lock);
    return shown;
}

bool LicenceService::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return busy;
}

juce::String LicenceService::message() const
{
    const juce::ScopedLock sl (lock);
    return lastMessage;
}

bool LicenceService::lastActionFailed() const
{
    const juce::ScopedLock sl (lock);
    return failed;
}

void LicenceService::publish (const juce::String& text, bool didFail)
{
    {
        const juce::ScopedLock sl (lock);
        shown = licence.record();
        lastMessage = text;
        failed = didFail;
    }
    sendChangeMessage();
}

void LicenceService::enqueue (std::function<void()> action)
{
    {
        const juce::ScopedLock sl (lock);
        if (isThreadRunning())
        {
            queue.push_back (std::move (action));
            busy = true;
            lastMessage = {};
            failed = false;
        }
        else
        {
            lastMessage = "This copy of KITNAP can't reach the licence server.";
            failed = true;
        }
    }
    sendChangeMessage();
    notify();
}

void LicenceService::activate (const juce::String& key)
{
    enqueue ([this, key]
    {
        const auto problem = licence.activate (key, juce::Time::getCurrentTime());
        publish (problem.isEmpty() ? juce::String ("KITNAP is activated on this computer. Thanks for buying it!") : problem,
                 problem.isNotEmpty());
    });
}

void LicenceService::deactivate()
{
    enqueue ([this]
    {
        const auto problem = licence.deactivate();
        publish (problem.isEmpty() ? juce::String ("KITNAP is deactivated on this computer: you can use your key on another one now.")
                                   : problem,
                 problem.isNotEmpty());
    });
}

void LicenceService::checkNow()
{
    enqueue ([this]
    {
        juce::String text;
        const auto result = licence.check (juce::Time::getCurrentTime(), text);
        publish (result == Licence::Check::good ? juce::String ("Checked: your licence is fine.") : text,
                 result != Licence::Check::good);
    });
}

void LicenceService::run()
{
    wait (5000); // not straight away: a host that is only scanning plugins closes KITNAP again quickly
    while (! threadShouldExit())
    {
        std::vector<std::function<void()>> todo;
        {
            const juce::ScopedLock sl (lock);
            todo.swap (queue);
        }
        for (auto& action : todo)
            if (! threadShouldExit())
                action();

        // Now and then, ask Polar whether the key is still good (not right after the person did
        // something: that has just talked to Polar). Without an answer nothing changes.
        if (todo.empty() && ! threadShouldExit() && licence.dueForCheck (juce::Time::getCurrentTime()))
        {
            juce::String text;
            if (licence.check (juce::Time::getCurrentTime(), text) == Licence::Check::removed)
                publish (text, true);
            else
                publish (message(), lastActionFailed());
        }

        bool more;
        {
            const juce::ScopedLock sl (lock);
            more = ! queue.empty();
            busy = more;
        }
        if (! more)
        {
            sendChangeMessage();
            wait (6 * 60 * 60 * 1000);
        }
    }
}

#if KITNAP_LICENCE_PREVIEW
void LicenceService::previewActivate (const juce::String& displayKey, const juce::String& label)
{
    {
        const juce::ScopedLock sl (lock);
        shown.key = shown.activationId = "preview";
        shown.displayKey = displayKey;
        shown.label = label;
        shown.checked = shown.lastSeen = juce::Time::getCurrentTime() - juce::RelativeTime::hours (3);
        lastMessage = {};
        failed = false;
    }
    sendChangeMessage();
}
#endif
} // namespace kitnap
