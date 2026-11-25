// The licence logic against a pretend Polar: activating (and every way it can go wrong), checking,
// working offline for a while, a licence file copied or edited, the clock set back, deactivating.
//
// Usage: KitnapLicenceTest
//        KitnapLicenceTest --live <api> <organization-id> <key>
//            Activates the key on the real Polar (use https://sandbox-api.polar.sh with a test key),
//            checks it, and deactivates it again.

#include "licence/Licence.h"
#include <iostream>

using namespace kitnap;

namespace
{
int failures = 0;

void expect (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (! ok)
        ++failures;
}

const juce::String org = "1dbfc517-0bbf-4301-9ba8-555ca42b9737";
const juce::String benefit = "8f3a2c10-5b7e-4d21-9c3e-2a1b0c9d8e7f";
const juce::String key = "KITNAP-1C285B2D-6CE6-4BC7-B8BE-ADB6A7E304DA";
const juce::String activationId = "b6724bc8-7ad9-4ca0-b143-7c896fcbb6fe";

struct FakePolar : HttpClient
{
    struct Call
    {
        juce::String action;
        juce::var body;
        juce::String url;
    };
    std::vector<Call> calls;
    std::function<HttpReply (const juce::String& action, const juce::var& body)> answer;

    HttpReply post (const juce::String& url, const juce::var& body) override
    {
        const auto action = url.fromLastOccurrenceOf ("/", false, false);
        calls.push_back ({ action, body, url });
        return answer ? answer (action, body) : HttpReply {};
    }
};

HttpReply reply (int status, const juce::String& json = {})
{
    return { status, juce::JSON::parse (json) };
}

// A licence key as Polar describes it (API version 2026-10).
juce::String licenceKeyJson (const juce::String& benefitId, const juce::String& status, const juce::String& expires)
{
    return R"({"id":"508176f7-2f0e-4b4e-8d0b-2b8c1d7c9e11","created_at":"2026-11-20T09:12:44.512331Z","modified_at":null,)"
           R"("organization_id":")" + org + R"(","customer_id":"0c6f1b2e-0a4d-4f4a-9a43-7d8e6b5c4a39",)"
           R"("customer":{"id":"0c6f1b2e-0a4d-4f4a-9a43-7d8e6b5c4a39","email":"buyer@example.com","name":"A Buyer"},)"
           R"("member_id":null,"member":null,"benefit_id":")" + benefitId + R"(","key":")" + key + R"(",)"
           R"("display_key":"****-E304DA","status":")" + status + R"(","limit_activations":2,"usage":0,"limit_usage":null,)"
           R"("validations":3,"last_validated_at":"2026-11-24T18:00:01.000001Z","expires_at":)" + expires + "}";
}

juce::String activationJson (const juce::String& benefitId = benefit, const juce::String& status = "granted",
                             const juce::String& expires = "null")
{
    return R"({"id":")" + activationId + R"(","license_key_id":"508176f7-2f0e-4b4e-8d0b-2b8c1d7c9e11","label":"Studio Mac",)"
           R"("meta":{"os":"Mac OSX"},"created_at":"2026-11-25T12:00:00.000000Z","modified_at":null,"license_key":)"
         + licenceKeyJson (benefitId, status, expires) + "}";
}

juce::String validatedJson (const juce::String& status = "granted")
{
    auto json = licenceKeyJson (benefit, status, "null").dropLastCharacters (1);
    return json + R"(,"activation":{"id":")" + activationId + R"(","license_key_id":"508176f7-2f0e-4b4e-8d0b-2b8c1d7c9e11",)"
                  R"("label":"Studio Mac","meta":{},"created_at":"2026-11-25T12:00:00Z","modified_at":null}})";
}

LicenceConfig config (const juce::String& benefitId = benefit)
{
    return { org, benefitId, "https://api.example.test/", "https://example.test/buy", "https://example.test/portal" };
}

const juce::Time t0 (2026, 10, 25, 12, 0, 0, 0, false); // 25 November 2026, 12:00 UTC
juce::Time after (double hours) { return t0 + juce::RelativeTime::hours (hours); }

int runLive (const juce::String& api, const juce::String& organization, const juce::String& liveKey)
{
    const auto file = juce::File::createTempFile (".json");
    WebHttpClient http;
    Licence licence ({ organization, {}, api, {}, {} }, file, "live-test-device", "KITNAP live test", http);
    const auto now = juce::Time::getCurrentTime();

    std::cout << "Activating on " << api << "\n";
    auto problem = licence.activate (liveKey, now);
    expect (problem.isEmpty(), "activated" + (problem.isEmpty() ? juce::String() : ": " + problem));
    if (problem.isNotEmpty())
        return 1;
    std::cout << "  activation " << licence.record().activationId << ", key " << licence.record().displayKey << "\n";

    juce::String message;
    expect (licence.check (now, message) == Licence::Check::good, "checked " + message);
    problem = licence.deactivate();
    expect (problem.isEmpty(), "deactivated " + problem);
    expect (licence.check (now, message) == Licence::Check::noAnswer && ! licence.record().valid(), "gone after deactivating");
    file.deleteFile();
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc == 5 && juce::String (argv[1]) == "--live")
        return runLive (argv[2], argv[3], argv[4]);

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KitnapLicenceTest");
    folder.deleteRecursively();
    const auto file = folder.getChildFile ("licence.json");
    FakePolar polar;

    std::cout << "No store set: no licence check\n";
    {
        Licence licence ({}, file, "device-a", "Studio Mac", polar);
        expect (licence.state (t0) == LicenceState::off, "off");
        expect (! licence.dueForCheck (after (1000)), "never checks");
    }

    std::cout << "Activating, and what goes wrong\n";
    {
        Licence licence (config(), file, "device-a", "Studio Mac", polar);
        expect (licence.state (t0) == LicenceState::none, "not activated at first");
        expect (licence.activate ("  \n", t0).contains ("Paste"), "an empty key asks for one, without asking Polar");
        expect (polar.calls.empty(), "  (no request)");

        const std::pair<HttpReply, const char*> failing[] {
            { reply (404, R"({"error":"ResourceNotFound","detail":"Not found"})"), "wasn't found" },
            { reply (403, R"({"error":"NotPermitted","detail":"License key activation limit already reached"})"), "two computers" },
            { reply (403, R"({"error":"NotPermitted","detail":"License key is no longer active."})"), "refunded" },
            { reply (422, R"({"detail":[{"loc":["body","key"],"msg":"Field required","type":"missing"}]})"), "doesn't look like" },
            { reply (0), "Can't reach" },
            { reply (500, "Internal Server Error"), "problem (500)" },
            { reply (429, R"({"detail":"Too many requests"})"), "Too many tries" },
        };
        for (auto& [answer, says] : failing)
        {
            polar.answer = [answer = answer] (auto&, auto&) { return answer; };
            const auto problem = licence.activate (key, t0);
            expect (problem.contains (says) && licence.state (t0) == LicenceState::none,
                    "HTTP " + juce::String (answer.status) + ": \"" + problem + "\"");
        }

        const auto& call = polar.calls.front();
        expect (call.url == "https://api.example.test/v1/customer-portal/license-keys/activate", "activates at " + call.url);
        expect (call.body["organization_id"] == org && call.body["key"] == key && call.body["label"] == "Studio Mac",
                "sends the organization, the key and this computer's name");
        expect (call.body["conditions"]["device"].toString().length() == 32 && call.body["meta"]["os"].toString().isNotEmpty(),
                "and this computer's tag: " + call.body["conditions"]["device"].toString());
        expect (! file.exists(), "nothing stored");

        polar.calls.clear();
        polar.answer = [] (auto&, auto&) { return reply (200, activationJson()); };
        const auto problem = licence.activate ("\"" + key + "\"\n", t0);
        expect (problem.isEmpty() && licence.state (t0) == LicenceState::active, "activated " + problem);
        expect (polar.calls.back().body["key"] == key, "with the quotes and line break around the pasted key dropped");
        expect (licence.record().displayKey == "****-E304DA" && licence.record().activationId == activationId
                    && licence.record().label == "Studio Mac",
                "remembers ****-E304DA, the activation and the computer");
        expect (file.existsAsFile() && ! file.loadFileAsString().contains ("device-a"), "stored, without the device itself");
    }

    std::cout << "The stored licence\n";
    {
        polar.calls.clear();
        Licence again (config(), file, "device-a", "Studio Mac", polar);
        expect (again.state (t0) == LicenceState::active && again.record().activationId == activationId, "reads back");
        Licence otherComputer (config(), file, "device-b", "Laptop", polar);
        expect (otherComputer.state (t0) == LicenceState::none, "copied to another computer: not activated there");
        Licence otherStore ({ "00000000-0000-0000-0000-000000000001", {}, "https://api.example.test", {}, {} }, file,
                            "device-a", "Studio Mac", polar);
        expect (otherStore.state (t0) == LicenceState::none, "another store's build doesn't count it");
        const auto original = file.loadFileAsString();
        file.replaceWithText (original.replace (juce::String (t0.toMilliseconds()), juce::String (after (24 * 300).toMilliseconds())));
        Licence edited (config(), file, "device-a", "Studio Mac", polar);
        expect (edited.state (t0) == LicenceState::none, "edited by hand: doesn't count");
        file.replaceWithText (original);
        expect (polar.calls.empty(), "none of that asks Polar");
    }

    std::cout << "Checking now and then, and offline\n";
    {
        Licence licence (config(), file, "device-a", "Studio Mac", polar);
        expect (! licence.dueForCheck (after (2)) && licence.dueForCheck (after (25)), "checks again after a day");

        polar.calls.clear();
        polar.answer = [] (auto&, auto&) { return reply (200, validatedJson()); };
        juce::String message;
        expect (licence.check (after (25), message) == Licence::Check::good, "good");
        const auto& body = polar.calls.back().body;
        expect (polar.calls.back().action == "validate" && body["activation_id"] == activationId && body["key"] == key
                    && body["benefit_id"] == benefit && body["organization_id"] == org,
                "validates this activation of the key, for KITNAP's benefit");
        expect (body["conditions"]["device"].toString().length() == 32, "on this computer");
        expect (licence.record().checked == after (25) && ! licence.dueForCheck (after (30)), "and counts from then");

        polar.answer = [] (auto&, auto&) { return reply (0); };
        expect (licence.check (after (24 * 10), message) == Licence::Check::noAnswer && message.contains ("Can't reach"),
                "offline: no answer");
        expect (licence.state (after (24 * 10)) == LicenceState::active, "still works offline after 10 days");
        expect (licence.state (after (24 * 30)) == LicenceState::active, "and after 29 days");
        expect (licence.state (after (24 * 32)) == LicenceState::needsCheck, "not after 31 days: needs a check");
        polar.answer = [] (auto&, auto&) { return reply (503, "Service Unavailable"); };
        expect (licence.check (after (24 * 32), message) == Licence::Check::noAnswer && licence.record().valid(),
                "Polar having a problem changes nothing");
        polar.answer = [] (auto&, auto&) { return reply (200, validatedJson()); };
        expect (licence.check (after (24 * 32), message) == Licence::Check::good
                    && licence.state (after (24 * 32)) == LicenceState::active,
                "back online: works again");

        licence.seen (after (24 * 40));
        expect (licence.state (after (24 * 36)) == LicenceState::needsCheck && licence.dueForCheck (after (24 * 36)),
                "clock set back 4 days: needs a check");
        expect (licence.state (after (24 * 39.5)) == LicenceState::active, "half a day back (time zones) is fine");
    }

    std::cout << "Turned off in Polar\n";
    {
        for (auto status : { 404, 403 })
        {
            file.deleteFile();
            Licence licence (config(), file, "device-a", "Studio Mac", polar);
            polar.answer = [] (auto&, auto&) { return reply (200, activationJson()); };
            licence.activate (key, t0);
            polar.answer = [status] (auto&, auto&) { return reply (status, R"({"error":"ResourceNotFound","detail":"Not found"})"); };
            juce::String message;
            expect (licence.check (after (25), message) == Licence::Check::removed && licence.state (after (25)) == LicenceState::none
                        && ! file.exists(),
                    "HTTP " + juce::String (status) + " on a check: not activated any more");
            expect (message.contains ("deactivated in your account"), "  \"" + message + "\"");
        }
        Licence licence (config(), file, "device-a", "Studio Mac", polar);
        polar.answer = [] (auto&, auto&) { return reply (200, activationJson()); };
        licence.activate (key, t0);
        polar.answer = [] (auto&, auto&) { return reply (200, validatedJson ("revoked")); };
        juce::String message;
        expect (licence.check (after (25), message) == Licence::Check::removed, "a check that says revoked: not activated any more");
    }

    std::cout << "Keys that don't belong\n";
    {
        file.deleteFile();
        Licence licence (config(), file, "device-a", "Studio Mac", polar);
        polar.calls.clear();
        polar.answer = [] (const juce::String& action, auto&)
        {
            return action == "activate" ? reply (200, activationJson ("11111111-2222-3333-4444-555555555555")) : reply (204);
        };
        auto problem = licence.activate (key, t0);
        expect (problem.contains ("another product") && licence.state (t0) == LicenceState::none && ! file.exists(),
                "a key for another product: \"" + problem + "\"");
        expect (polar.calls.size() == 2 && polar.calls[1].action == "deactivate" && polar.calls[1].body["activation_id"] == activationId,
                "  and its place is given back");

        polar.answer = [] (const juce::String& action, auto&)
        {
            return action == "activate" ? reply (200, activationJson (benefit, "granted", "\"2026-06-01T00:00:00.654321+00:00\"")) : reply (204);
        };
        problem = licence.activate (key, t0);
        expect (problem.contains ("expired") && licence.state (t0) == LicenceState::none, "an expired key (Polar's microseconds read): \"" + problem + "\"");

        polar.answer = [] (auto&, auto&) { return reply (200, activationJson (benefit, "granted", "\"2027-06-01T00:00:00Z\"")); };
        expect (licence.activate (key, t0).isEmpty() && licence.state (t0) == LicenceState::active,
                "one that expires next year is fine");

        Licence anyBenefit (config ({}), file, "device-a", "Studio Mac", polar);
        file.deleteFile();
        polar.answer = [] (auto&, auto&) { return reply (200, activationJson ("11111111-2222-3333-4444-555555555555")); };
        expect (anyBenefit.activate (key, t0).isEmpty(), "without a benefit set, any key of the store works");
    }

    std::cout << "Deactivating\n";
    {
        Licence licence (config(), file, "device-a", "Studio Mac", polar);
        polar.answer = [] (auto&, auto&) { return reply (200, activationJson()); };
        licence.activate (key, t0);

        polar.answer = [] (auto&, auto&) { return reply (0); };
        auto problem = licence.deactivate();
        expect (problem.contains ("needs it") && licence.state (t0) == LicenceState::active, "offline: stays activated");
        polar.calls.clear();
        polar.answer = [] (auto&, auto&) { return reply (204); };
        problem = licence.deactivate();
        expect (problem.isEmpty() && licence.state (t0) == LicenceState::none && ! file.exists(), "deactivated");
        expect (polar.calls.back().action == "deactivate" && polar.calls.back().body["activation_id"] == activationId
                    && polar.calls.back().body["key"] == key && polar.calls.back().body["organization_id"] == org,
                "  this activation");

        polar.answer = [] (auto&, auto&) { return reply (200, activationJson()); };
        licence.activate (key, t0);
        polar.answer = [] (auto&, auto&) { return reply (404, R"({"error":"ResourceNotFound","detail":"Not found"})"); };
        expect (licence.deactivate().isEmpty() && licence.state (t0) == LicenceState::none, "already gone in Polar: deactivated");
    }

    folder.deleteRecursively();
    std::cout << (failures == 0 ? "All licence checks passed.\n" : juce::String (failures) + " licence checks FAILED.\n");
    return failures == 0 ? 0 : 1;
}
