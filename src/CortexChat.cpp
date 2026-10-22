/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexChat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Channel.h"
#include "ChannelMgr.h"
#include "Config.h"
#include "CortexBots.h"
#include "CortexChatPrompt.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"

using namespace CortexChat;

namespace
{
    std::atomic<std::shared_ptr<Config const>> sConfig{std::make_shared<Config const>()};

    std::shared_ptr<Config const> Cfg() { return sConfig.load(std::memory_order_acquire); }

    uint64 NowMs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    // --- Requests and replies -------------------------------------------------------------------------------------

    struct Job
    {
        Kind kind = Kind::WHISPER;
        ObjectGuid bot;
        ObjectGuid from;
        std::string botName;
        std::string scope;
        std::string channelName;
        std::string user;
        uint64 queuedMs = 0;
        std::shared_ptr<Config const> cfg;
    };

    struct Reply
    {
        Job job;
        CortexChatClient::Result result;
        uint64 latencyMs = 0;
        std::vector<std::string> parts;
        std::size_t next = 0;
        uint64 deliverAtMs = 0;
    };

    // Shared with the workers.
    std::mutex sJobsMutex;
    std::condition_variable sJobsCv;
    std::deque<Job> sJobs;
    std::mutex sDoneMutex;
    std::vector<Reply> sDone;
    std::vector<std::thread> sWorkers;
    std::atomic<bool> sStop{false};

    // World thread only.
    std::vector<Reply> sOutgoing;
    std::unordered_set<ObjectGuid> sBusy;  // bots with a request in flight or a reply being typed
    std::unordered_map<ObjectGuid::LowType, uint64> sPlayerLast;
    std::deque<uint64> sLastMinute;
    uint32 sDay = 0;
    uint32 sDayCount = 0;
    uint64 sNextStatsMs = 0;
    uint64 sNextPruneMs = 0;
    uint64 sNextFailLogMs = 0;

    struct Stats
    {
        uint32 requests = 0, sent = 0, skipped = 0, failed = 0, refused = 0, dropped = 0;
        uint32 busy = 0, cooldown = 0, limited = 0, full = 0;
        uint64 latencyMs = 0, inputTokens = 0, outputTokens = 0;
        uint32 replies = 0;
    } sStats;

    // The bot body's chat command names, lowercase.
    std::unordered_set<std::string> sCommands;

    // --- Chat history per conversation ----------------------------------------------------------------------------

    struct Line
    {
        uint64 ms;
        ObjectGuid speaker;
        std::string name;
        std::string text;
    };
    std::unordered_map<std::string, std::deque<Line>> sHistory;

    void AddHistory(std::string const& scope, Player* speaker, std::string const& text)
    {
        auto cfg = Cfg();
        auto& lines = sHistory[scope];
        lines.push_back({NowMs(), speaker->GetGUID(), speaker->GetName(), text});
        while (lines.size() > cfg->historyLines)
            lines.pop_front();
    }

    std::string History(std::string const& scope, Player* bot)
    {
        auto it = sHistory.find(scope);
        if (it == sHistory.end())
            return {};
        uint64 oldest = NowMs() - uint64(Cfg()->historyMinutes) * MINUTE * IN_MILLISECONDS;
        std::string text;
        for (Line const& line : it->second)
        {
            if (line.ms < oldest)
                continue;
            text += (line.speaker == bot->GetGUID() ? std::string("You") : line.name) + ": " + line.text + "\n";
        }
        return text;
    }

    void PruneHistory()
    {
        uint64 oldest = NowMs() - uint64(Cfg()->historyMinutes) * MINUTE * IN_MILLISECONDS;
        for (auto it = sHistory.begin(); it != sHistory.end();)
        {
            while (!it->second.empty() && it->second.front().ms < oldest)
                it->second.pop_front();
            it = it->second.empty() ? sHistory.erase(it) : std::next(it);
        }
        for (auto it = sPlayerLast.begin(); it != sPlayerLast.end();)
            it = it->second < oldest ? sPlayerLast.erase(it) : std::next(it);
    }

    // --- Which messages are chat ----------------------------------------------------------------------------------

    // Would the bot body run this as a command from someone allowed to command the bot? Mirrors how the body parses
    // chat commands (the text, then each shorter run of its leading words, as a command name), except that a
    // question is talk: "who are you?" isn't the "who" command.
    bool LooksLikeCommand(std::string const& text)
    {
        std::string lower = Lower(text);
        if (!CortexBots::CommandPrefix().empty())
            return lower.starts_with(Lower(CortexBots::CommandPrefix()));
        if (!CortexBots::CommandSeparator().empty() && text.find(CortexBots::CommandSeparator()) != std::string::npos)
            return true;
        if (lower.find("|h") != std::string::npos)  // item, quest and spell links
            return true;
        if (lower.starts_with('#') || lower.starts_with("do ") || lower.starts_with("d ") || lower.starts_with("debug "))
            return true;
        if (lower.ends_with('?'))
            return false;
        for (std::string prefix = lower;;)
        {
            if (sCommands.count(prefix))
                return true;
            std::size_t space = prefix.rfind(' ');
            if (space == std::string::npos || !space)
                return false;
            prefix.resize(space);
        }
    }

    // Chat (ours), or a command (the body's)? Someone who can't command the bot gets chat for anything but the few
    // commands everyone may give; the body would only refuse the rest.
    bool IsChat(Player* bot, Player* from, std::string const& msg)
    {
        if (!CortexBots::IsBot(bot) || msg.empty() || msg.starts_with("BOT\t"))
            return false;
        if (CortexBots::IsAllowedCommand(bot, msg))
            return false;
        if (!CortexBots::TakesCommandsFrom(bot, from))
            return true;
        return !LooksLikeCommand(msg);
    }

    // --- Who answers -----------------------------------------------------------------------------------------------

    std::unordered_set<std::string> Words(std::string const& msg)
    {
        std::unordered_set<std::string> words;
        std::string word;
        for (char c : msg)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            else if (!word.empty())
            {
                words.insert(word);
                word.clear();
            }
        }
        if (!word.empty())
            words.insert(word);
        return words;
    }

    bool Named(std::unordered_set<std::string> const& words, Player* bot) { return words.count(Lower(bot->GetName())); }

    bool IsQuestion(std::string const& msg)
    {
        if (msg.find('?') != std::string::npos)
            return true;
        std::string lower = Lower(msg);
        for (char const* start : {"anyone", "any1", "how ", "where ", "what ", "who ", "which ", "does ", "do ", "can ",
                                  "is ", "lf"})
            if (lower.starts_with(start))
                return true;
        return false;
    }

    bool OneOnOne(Group const* group) { return group && group->GetMembersCount() == 2; }

    // Named in the message wins; otherwise one of them by chance (`chance` in percent). `addressed` tells which.
    Player* Choose(std::vector<Player*> const& candidates, std::string const& msg, uint32 chance, bool& addressed)
    {
        addressed = false;
        if (candidates.empty())
            return nullptr;
        auto words = Words(msg);
        for (Player* candidate : candidates)
            if (Named(words, candidate))
            {
                addressed = true;
                return candidate;
            }
        if (!roll_chance_i(static_cast<int>(chance)))
            return nullptr;
        return candidates[urand(0, candidates.size() - 1)];
    }

    // --- Requests --------------------------------------------------------------------------------------------------

    std::string Heading(Kind kind, Player* from, Player* bot, std::string const& channelName)
    {
        std::ostringstream out;
        switch (kind)
        {
            case Kind::WHISPER:
                out << from->GetName() << " whispers to you:";
                break;
            case Kind::SAY:
                out << from->GetName() << " says out loud, " << uint32(from->GetDistance(bot)) << " yards from you:";
                break;
            case Kind::PARTY:
                out << "In party chat, " << from->GetName() << " writes:";
                break;
            case Kind::GUILD:
                out << "In guild chat, " << from->GetName() << " writes:";
                break;
            case Kind::CHANNEL:
                out << "In the " << channelName << " channel, " << from->GetName() << " writes:";
                break;
        }
        return out.str();
    }

    bool UnderLimits(Config const& cfg, Player* from, Player* bot)
    {
        uint64 now = NowMs();
        if (sBusy.count(bot->GetGUID()))
        {
            ++sStats.busy;
            return false;
        }
        auto last = sPlayerLast.find(from->GetGUID().GetCounter());
        if (last != sPlayerLast.end() && now - last->second < cfg.playerCooldownMs)
        {
            ++sStats.cooldown;
            return false;
        }
        while (!sLastMinute.empty() && now - sLastMinute.front() >= MINUTE * IN_MILLISECONDS)
            sLastMinute.pop_front();
        uint32 day = static_cast<uint32>(time(nullptr) / DAY);
        if (day != sDay)
        {
            sDay = day;
            sDayCount = 0;
        }
        if (sLastMinute.size() >= cfg.maxPerMinute || sDayCount >= cfg.maxPerDay)
        {
            ++sStats.limited;
            return false;
        }
        return true;
    }

    // `msg` goes into the scope's history whether or not a bot answers it.
    void Ask(Kind kind, Player* from, Player* bot, std::string const& msg, bool addressed, std::string const& scope,
             std::string const& channelName = {})
    {
        auto cfg = Cfg();
        if (!bot || !UnderLimits(*cfg, from, bot))
        {
            AddHistory(scope, from, msg);
            return;
        }

        std::string user = CortexChatPrompt::AboutBot(bot);
        user += "\nWho's talking:\n" + CortexChatPrompt::AboutSpeaker(from, bot);
        std::string history = History(scope, bot);
        if (!history.empty())
            user += "\nEarlier in this conversation (oldest first):\n" + history;
        user += "\n" + Heading(kind, from, bot, channelName) + "\n" + msg + "\n\n";
        if (kind == Kind::WHISPER)
            user += "Reply to the whisper.";
        else if (kind == Kind::PARTY && OneOnOne(bot->GetGroup()))
            user += "It's just the two of you in the party, so this is meant for you. Reply to it in party chat. If "
                    "it's a question you know the answer to, answer it; if you don't know, say so.";
        else if (addressed)
            user += "They're talking to you.";
        else
            user += "It isn't addressed to you by name. Answer only if a player in your place naturally would (a "
                    "question you can help with, or a greeting at a quiet moment); otherwise reply [skip].";
        AddHistory(scope, from, msg);

        Job job;
        job.kind = kind;
        job.bot = bot->GetGUID();
        job.from = from->GetGUID();
        job.botName = bot->GetName();
        job.scope = scope;
        job.channelName = channelName;
        job.user = std::move(user);
        job.queuedMs = NowMs();
        job.cfg = cfg;
        {
            std::lock_guard<std::mutex> lock(sJobsMutex);
            if (sJobs.size() >= cfg->queueSize)
            {
                ++sStats.full;
                return;
            }
            if (cfg->logPrompts)
                LOG_INFO("module.cortexchat", "CortexChat: {} -> {}\n{}", from->GetName(), bot->GetName(), job.user);
            sJobs.push_back(std::move(job));
        }
        sJobsCv.notify_one();

        sBusy.insert(bot->GetGUID());
        sPlayerLast[from->GetGUID().GetCounter()] = NowMs();
        sLastMinute.push_back(NowMs());
        ++sDayCount;
        ++sStats.requests;
    }

    void Work()
    {
        while (true)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(sJobsMutex);
                sJobsCv.wait(lock, [] { return sStop.load() || !sJobs.empty(); });
                if (sStop.load())
                    return;
                job = std::move(sJobs.front());
                sJobs.pop_front();
            }

            Reply reply;
            reply.job = std::move(job);
            Config const& cfg = *reply.job.cfg;
            uint64 start = NowMs();
            if (start - reply.job.queuedMs > cfg.maxReplyAgeMs)
                reply.result.error = "waited too long in the queue";
            else
                reply.result = CortexChatClient::Complete(cfg.client, cfg.systemPrompt, reply.job.user, sStop);
            reply.latencyMs = NowMs() - start;
            if (reply.result.ok && NowMs() - reply.job.queuedMs > cfg.maxReplyAgeMs)
            {
                reply.result.ok = false;
                reply.result.error = "reply came too late";
            }

            std::lock_guard<std::mutex> lock(sDoneMutex);
            sDone.push_back(std::move(reply));
        }
    }

    // --- Delivery --------------------------------------------------------------------------------------------------

    // At most two chat lines of `maxLength`, cut after a sentence where there is one.
    std::vector<std::string> Split(std::string text, std::size_t maxLength)
    {
        std::vector<std::string> parts;
        for (int i = 0; i < 2 && !text.empty(); ++i)
        {
            if (text.size() <= maxLength)
            {
                parts.push_back(text);
                break;
            }
            std::size_t cut = std::string::npos;
            for (std::size_t pos = maxLength; pos > maxLength / 2; --pos)
                if ((text[pos - 1] == '.' || text[pos - 1] == '!' || text[pos - 1] == '?') && text[pos] == ' ')
                {
                    cut = pos;
                    break;
                }
            if (cut == std::string::npos)
                cut = text.rfind(' ', maxLength);
            if (cut == std::string::npos || !cut)
                cut = maxLength;
            parts.push_back(text.substr(0, cut));
            text.erase(0, cut);
            text.erase(0, text.find_first_not_of(' '));
        }
        return parts;
    }

    uint64 TypingMs(Config const& cfg, std::string const& text)
    {
        return std::min<uint64>(cfg.maxTypingMs, uint64(cfg.typingMsPerChar) * text.size());
    }

    // Says the next part; false when the bot is done (all said, or it can't say it any more).
    bool Deliver(Reply& reply)
    {
        Job const& job = reply.job;
        Player* bot = ObjectAccessor::FindPlayer(job.bot);
        if (!CortexBots::IsBot(bot))
        {
            ++sStats.dropped;
            return false;
        }

        std::string const& part = reply.parts[reply.next++];
        bool said = false;
        switch (job.kind)
        {
            case Kind::WHISPER:
                if (Player* to = ObjectAccessor::FindConnectedPlayer(job.from))
                    said = CortexBots::Whisper(bot, part, to->GetName());
                break;
            case Kind::SAY:
                said = CortexBots::Say(bot, part);
                break;
            case Kind::PARTY:
                said = CortexBots::SayToParty(bot, part);
                break;
            case Kind::GUILD:
                said = CortexBots::SayToGuild(bot, part);
                break;
            case Kind::CHANNEL:
                // Channel::Say leaves out a bot that isn't on the channel (it left the zone meanwhile).
                if (ChannelMgr* channels = ChannelMgr::forTeam(bot->GetTeamId()))
                    if (Channel* channel = channels->GetChannel(job.channelName, bot, false))
                    {
                        channel->Say(bot->GetGUID(), part, LANG_UNIVERSAL);
                        CortexBots::ChatTelemetry(bot, "channel", part);
                        said = true;
                    }
                break;
        }
        if (!said)
        {
            ++sStats.dropped;
            return false;
        }
        ++sStats.sent;
        AddHistory(job.scope, bot, part);
        return reply.next < reply.parts.size();
    }

    void LogFailure(Reply const& reply)
    {
        uint64 now = NowMs();
        if (now < sNextFailLogMs)
            return;
        sNextFailLogMs = now + 10 * IN_MILLISECONDS;
        LOG_WARN("module.cortexchat", "CortexChat: no reply from {} for {} ({} tries, {} ms): {}",
                 CortexChatClient::ProviderName(reply.job.cfg->client.provider), reply.job.botName,
                 reply.result.attempts, reply.latencyMs, reply.result.error);
    }

    void LogStats()
    {
        Stats& s = sStats;
        if (s.requests || s.busy || s.cooldown || s.limited || s.full)
            LOG_INFO("module.cortexchat",
                     "CortexChat: {} requests, {} lines said, {} skipped, {} failed, {} refused, {} dropped; not asked: "
                     "{} busy, {} cooldown, {} over limit, {} queue full; avg {} ms, tokens {} in / {} out; {} today",
                     s.requests, s.sent, s.skipped, s.failed, s.refused, s.dropped, s.busy, s.cooldown, s.limited,
                     s.full, s.replies ? s.latencyMs / s.replies : 0, s.inputTokens, s.outputTokens, sDayCount);
        s = Stats();
    }
}

void CortexChat::LoadConfig()
{
    auto cfg = std::make_shared<Config>();
    cfg->enabled = sConfigMgr->GetOption<bool>("CortexChat.Enable", false);

    CortexChatClient::Settings& client = cfg->client;
    std::string provider = Lower(sConfigMgr->GetOption<std::string>("CortexChat.Provider", "anthropic"));
    if (!CortexChatClient::ParseProvider(provider, client.provider))
    {
        LOG_ERROR("module.cortexchat", "CortexChat: unknown CortexChat.Provider '{}', chat is off", provider);
        cfg->enabled = false;
    }
    client.url = sConfigMgr->GetOption<std::string>("CortexChat.Url", "https://api.anthropic.com/v1/messages");
    client.model = sConfigMgr->GetOption<std::string>("CortexChat.Model", "claude-haiku-5-5");
    client.apiKey = sConfigMgr->GetOption<std::string>("CortexChat.ApiKey", "", false);
    client.maxTokens = sConfigMgr->GetOption<uint32>("CortexChat.MaxTokens", 300);
    client.thinking = sConfigMgr->GetOption<std::string>("CortexChat.Anthropic.Thinking", "disabled");
    client.effort = sConfigMgr->GetOption<std::string>("CortexChat.Anthropic.Effort", "low");
    client.openAiMaxTokensField = sConfigMgr->GetOption<std::string>("CortexChat.OpenAI.MaxTokensField", "max_tokens");
    client.connectTimeoutMs = sConfigMgr->GetOption<uint32>("CortexChat.ConnectTimeoutMs", 5000);
    client.timeoutMs = sConfigMgr->GetOption<uint32>("CortexChat.TimeoutMs", 20000);
    client.retries = sConfigMgr->GetOption<uint32>("CortexChat.Retries", 1);
    client.maxRetryWaitMs = sConfigMgr->GetOption<uint32>("CortexChat.MaxRetryWaitMs", 8000);
    client.caCertFile = sConfigMgr->GetOption<std::string>("CortexChat.CaCertFile", "/etc/ssl/certs/ca-certificates.crt");
    if (cfg->enabled && client.apiKey.empty() && client.provider != CortexChatClient::Provider::OLLAMA)
    {
        LOG_ERROR("module.cortexchat", "CortexChat: no API key (CortexChat.ApiKey, or env AC_CORTEX_CHAT_API_KEY), "
                                       "chat is off");
        cfg->enabled = false;
    }

    cfg->workers = std::max(1u, sConfigMgr->GetOption<uint32>("CortexChat.Workers", 2));
    cfg->queueSize = sConfigMgr->GetOption<uint32>("CortexChat.QueueSize", 32);
    cfg->maxPerMinute = sConfigMgr->GetOption<uint32>("CortexChat.MaxRequestsPerMinute", 20);
    cfg->maxPerDay = sConfigMgr->GetOption<uint32>("CortexChat.MaxRequestsPerDay", 3000);
    cfg->playerCooldownMs = sConfigMgr->GetOption<uint32>("CortexChat.PlayerCooldownMs", 1500);
    cfg->maxReplyAgeMs = sConfigMgr->GetOption<uint32>("CortexChat.MaxReplyAgeMs", 45000);
    cfg->readDelayMs = sConfigMgr->GetOption<uint32>("CortexChat.ReadDelayMs", 1500);
    cfg->typingMsPerChar = sConfigMgr->GetOption<uint32>("CortexChat.TypingMsPerChar", 50);
    cfg->maxTypingMs = sConfigMgr->GetOption<uint32>("CortexChat.MaxTypingMs", 10000);
    cfg->maxReplyLength = std::clamp(sConfigMgr->GetOption<uint32>("CortexChat.MaxReplyLength", 250), 40u, 255u);

    cfg->whisper = sConfigMgr->GetOption<bool>("CortexChat.Whisper.Enable", true);
    cfg->say = sConfigMgr->GetOption<bool>("CortexChat.Say.Enable", true);
    cfg->sayRange = sConfigMgr->GetOption<float>("CortexChat.Say.Range", 25.0f);
    cfg->sayChance = sConfigMgr->GetOption<uint32>("CortexChat.Say.ReplyChance", 50);
    cfg->party = sConfigMgr->GetOption<bool>("CortexChat.Party.Enable", true);
    cfg->partyChance = sConfigMgr->GetOption<uint32>("CortexChat.Party.ReplyChance", 70);
    cfg->guild = sConfigMgr->GetOption<bool>("CortexChat.Guild.Enable", true);
    cfg->guildChance = sConfigMgr->GetOption<uint32>("CortexChat.Guild.ReplyChance", 40);
    cfg->channel = sConfigMgr->GetOption<bool>("CortexChat.Channel.Enable", true);
    cfg->channelIds.clear();
    std::stringstream ids(sConfigMgr->GetOption<std::string>("CortexChat.Channel.Ids", "1"));
    for (std::string id; std::getline(ids, id, ',');)
    {
        id.erase(std::remove(id.begin(), id.end(), ' '), id.end());
        if (!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); }))
            cfg->channelIds.insert(static_cast<uint32>(std::stoul(id)));
    }
    cfg->channelQuestionChance = sConfigMgr->GetOption<uint32>("CortexChat.Channel.QuestionChance", 70);
    cfg->channelChance = sConfigMgr->GetOption<uint32>("CortexChat.Channel.ReplyChance", 10);

    cfg->historyLines = sConfigMgr->GetOption<uint32>("CortexChat.History.Lines", 8);
    cfg->historyMinutes = sConfigMgr->GetOption<uint32>("CortexChat.History.Minutes", 20);
    cfg->extraInstructions = sConfigMgr->GetOption<std::string>("CortexChat.ExtraInstructions", "");
    cfg->systemPrompt = CortexChatPrompt::System(cfg->extraInstructions);
    cfg->logPrompts = sConfigMgr->GetOption<bool>("CortexChat.LogPrompts", false);

    if (cfg->enabled)
        LOG_INFO("module.cortexchat", "CortexChat: on, {} {} at {}", CortexChatClient::ProviderName(client.provider),
                 client.model, client.url);
    sConfig.store(std::move(cfg), std::memory_order_release);
}

void CortexChat::Start()
{
    CortexChatPrompt::LoadPersonas();

    for (std::string& name : CortexBots::CommandNames())
        sCommands.insert(std::move(name));

    sStop = false;
    for (uint32 i = 0; i < Cfg()->workers; ++i)
        sWorkers.emplace_back(Work);
}

void CortexChat::Stop()
{
    sStop = true;
    sJobsCv.notify_all();
    for (std::thread& worker : sWorkers)
        if (worker.joinable())
            worker.join();
    sWorkers.clear();
}

void CortexChat::Update()
{
    uint64 now = NowMs();

    std::vector<Reply> done;
    {
        std::lock_guard<std::mutex> lock(sDoneMutex);
        done.swap(sDone);
    }
    for (Reply& reply : done)
    {
        Config const& cfg = *reply.job.cfg;
        CortexChatClient::Result const& result = reply.result;
        sStats.latencyMs += reply.latencyMs;
        sStats.inputTokens += result.inputTokens;
        sStats.outputTokens += result.outputTokens;
        ++sStats.replies;
        std::string text = result.ok ? CortexChatPrompt::Clean(result.text, reply.job.botName) : std::string();
        if (cfg.logPrompts && result.ok)
            LOG_INFO("module.cortexchat", "CortexChat: {} ({} ms): {}", reply.job.botName, reply.latencyMs,
                     result.text);
        if (!result.ok || text.empty())
        {
            if (result.refused)
                ++sStats.refused;
            else if (!result.ok)
            {
                ++sStats.failed;
                LogFailure(reply);
            }
            else
                ++sStats.skipped;
            sBusy.erase(reply.job.bot);
            continue;
        }
        reply.parts = Split(text, cfg.maxReplyLength);
        reply.deliverAtMs =
            std::max(now, reply.job.queuedMs + cfg.readDelayMs + TypingMs(cfg, reply.parts.front()));
        sOutgoing.push_back(std::move(reply));
    }

    for (auto it = sOutgoing.begin(); it != sOutgoing.end();)
    {
        if (it->deliverAtMs > now)
        {
            ++it;
            continue;
        }
        if (Deliver(*it))
        {
            it->deliverAtMs = now + TypingMs(*it->job.cfg, it->parts[it->next]);
            ++it;
        }
        else
        {
            sBusy.erase(it->job.bot);
            it = sOutgoing.erase(it);
        }
    }

    if (now >= sNextPruneMs)
    {
        sNextPruneMs = now + 5 * MINUTE * IN_MILLISECONDS;
        PruneHistory();
    }
    if (now >= sNextStatsMs)
    {
        if (sNextStatsMs)
            LogStats();
        sNextStatsMs = now + 10 * MINUTE * IN_MILLISECONDS;
    }
}

void CortexChat::OnWhisper(Player* from, Player* bot, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !cfg->whisper || !CortexBots::IsBot(bot) || !IsChat(bot, from, msg))
        return;
    std::string scope = "w:" + std::to_string(bot->GetGUID().GetCounter()) + ":" +
                        std::to_string(from->GetGUID().GetCounter());
    Ask(Kind::WHISPER, from, bot, msg, true, scope);
}

void CortexChat::OnSay(Player* from, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !cfg->say || msg.empty() || !from->IsInWorld())
        return;

    // Bots in earshot that understand the speaker: same faction, same phase.
    std::vector<Player*> candidates;
    Player* targeted = nullptr;
    for (auto const& ref : from->GetMap()->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || player == from || !CortexBots::IsBot(player) || player->GetTeamId() != from->GetTeamId() ||
            !player->InSamePhase(from) || !player->IsWithinDistInMap(from, cfg->sayRange))
            continue;
        candidates.push_back(player);
        if (player->GetGUID() == from->GetTarget())
            targeted = player;
    }
    if (candidates.empty())
        return;

    bool addressed = targeted != nullptr;
    Player* bot = targeted;
    if (!bot)
    {
        bot = Choose(candidates, msg, 0, addressed);
        if (!bot && roll_chance_i(static_cast<int>(cfg->sayChance)))
            bot = *std::min_element(candidates.begin(), candidates.end(),
                                    [from](Player* a, Player* b) { return a->GetExactDist(from) < b->GetExactDist(from); });
    }
    if (!bot || !IsChat(bot, from, msg))
        return;
    std::string scope = "s:" + std::to_string(bot->GetGUID().GetCounter()) + ":" +
                        std::to_string(from->GetGUID().GetCounter());
    Ask(Kind::SAY, from, bot, msg, addressed, scope);
}

void CortexChat::OnParty(Player* from, Group* group, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !cfg->party || !group || msg.empty())
        return;

    std::vector<Player*> candidates;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != from && member->IsInWorld() && CortexBots::IsBot(member) && IsChat(member, from, msg))
            candidates.push_back(member);
    }
    std::string scope = "p:" + std::to_string(group->GetGUID().GetCounter());
    bool addressed = false;
    // Just the player and one bot: everything said in party chat is meant for that bot.
    Player* bot = nullptr;
    if (OneOnOne(group) && candidates.size() == 1)
    {
        bot = candidates.front();
        addressed = true;
    }
    else
        bot = Choose(candidates, msg, cfg->partyChance, addressed);
    if (!bot)
    {
        AddHistory(scope, from, msg);
        return;
    }
    Ask(Kind::PARTY, from, bot, msg, addressed, scope);
}

void CortexChat::OnGuild(Player* from, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !cfg->guild || !from->GetGuildId() || msg.empty())
        return;

    std::vector<Player*> candidates;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        if (player && player != from && player->GetGuildId() == from->GetGuildId() && player->IsInWorld() &&
            CortexBots::IsBot(player) && IsChat(player, from, msg))
            candidates.push_back(player);
    std::string scope = "g:" + std::to_string(from->GetGuildId());
    bool addressed = false;
    Player* bot = Choose(candidates, msg, cfg->guildChance, addressed);
    if (!bot)
    {
        AddHistory(scope, from, msg);
        return;
    }
    Ask(Kind::GUILD, from, bot, msg, addressed, scope);
}

void CortexChat::OnChannel(Player* from, Channel* channel, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !cfg->channel || !channel || msg.empty() || !cfg->channelIds.count(channel->GetChannelId()))
        return;

    // Bots on the channel: the zone's for General, Trade and LocalDefense (bots join their zone's channels; Channel
    // keeps its member list private), anyone's for the realm-wide ones (LookingForGroup ...). For a question, bots at least the asker's level
    // first (they've been there).
    // ChatChannels.dbc ids of the realm-wide ones: WorldDefense, GuildRecruitment, LookingForGroup.
    uint32 channelId = channel->GetChannelId();
    bool zoneChannel = channelId != 23 && channelId != 25 && channelId != 26;
    std::vector<Player*> candidates, seasoned;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!player || player == from || player->GetTeamId() != from->GetTeamId() || !player->IsInWorld() ||
            !CortexBots::IsBot(player) || (zoneChannel && player->GetZoneId() != from->GetZoneId()))
            continue;
        candidates.push_back(player);
        if (player->GetLevel() >= from->GetLevel())
            seasoned.push_back(player);
    }
    std::string scope = "c:" + std::to_string(from->GetTeamId()) + ":" + channel->GetName();
    bool question = IsQuestion(msg);
    uint32 chance = question ? cfg->channelQuestionChance : cfg->channelChance;
    bool addressed = false;
    Player* bot = Choose(candidates, msg, 0, addressed);
    if (!bot && roll_chance_i(static_cast<int>(chance)))
    {
        auto const& pool = question && !seasoned.empty() ? seasoned : candidates;
        if (!pool.empty())
            bot = pool[urand(0, pool.size() - 1)];
    }
    if (!bot)
    {
        AddHistory(scope, from, msg);
        return;
    }
    Ask(Kind::CHANNEL, from, bot, msg, addressed, scope, channel->GetName());
}

bool CortexChat::Claims(Player* bot, Player* from, uint32 type, std::string const& msg)
{
    auto cfg = Cfg();
    if (!cfg->enabled || !bot || !from || CortexBots::IsBot(from))
        return false;
    switch (type)
    {
        case CHAT_MSG_WHISPER:
            if (!cfg->whisper)
                return false;
            break;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            if (!cfg->party)
                return false;
            break;
        case CHAT_MSG_GUILD:
            if (!cfg->guild)
                return false;
            break;
        default:
            return false;
    }
    return IsChat(bot, from, msg);
}

bool CortexChat::Silences(Player* /*bot*/, uint32 type)
{
    auto cfg = Cfg();
    if (!cfg->enabled)
        return false;
    switch (type)
    {
        case CHAT_MSG_WHISPER:
            return cfg->whisper;
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
            return cfg->say;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            return cfg->party;
        case CHAT_MSG_GUILD:
            return cfg->guild;
        case CHAT_MSG_CHANNEL:
            return cfg->channel;
        default:
            return false;
    }
}
