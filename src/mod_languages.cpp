/*
 * mod-languages - buy a language from the NPC who speaks it
 *
 * Talk to a faction leader and, for a fee, they will teach you their people's
 * language. Which NPC teaches what is data, not code: the world database table
 * `language_teacher` maps a creature entry to a language spell and a price,
 * seeded with the ten city leaders in sql/05_language_teachers.sql.
 *
 * The option is appended to the leader's own gossip window. The core builds
 * gossip menus from the database and gives every database option the same
 * sender and action (0), so a database row could not be told apart on
 * selection; instead CanCreatureGossipHello is used, which runs before both
 * the creature's own script and the core's default handling
 * (ScriptMgr::OnGossipHello). The leader's normal menu is rebuilt first with
 * Player::PrepareGossipMenu, so quests and existing options stay where they
 * are, and the language option is added on top with its own sender so the
 * selection is unambiguous.
 *
 * None of the seeded leaders has a CreatureScript gossip hello of its own, so
 * nothing is being shadowed. An NPC added to the table later might, in which
 * case its script menu would be replaced while the option is on offer.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr uint32 SENDER_LANGUAGE = 0xFA01;

    struct LanguageOffer
    {
        std::string name;             // what to call it; empty falls back to the spell name
        uint32 spell = 0;
        uint32 cost = 0;              // in copper
        uint32 requiredFaction = 0;   // Faction.dbc id, 0 = no requirement
        uint8  requiredRank = 0;      // ReputationRank, e.g. REP_EXALTED
    };

    std::unordered_map<uint32, std::vector<LanguageOffer>> teachers;   // creature entry -> offers

    struct LanguagesConfig
    {
        bool Enable = true;
        bool Announce = true;
        bool RequireReputation = true;
    };

    LanguagesConfig cfg;

    void LoadConfig()
    {
        cfg.Enable = sConfigMgr->GetOption<bool>("Languages.Enable", true);
        cfg.Announce = sConfigMgr->GetOption<bool>("Languages.Announce", true);
        cfg.RequireReputation = sConfigMgr->GetOption<bool>("Languages.RequireReputation", true);
    }

    void LoadTeachers()
    {
        teachers.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT CreatureEntry, Spell, Cost, RequiredFaction, RequiredRank, Name FROM language_teacher");
        if (!result)
        {
            LOG_WARN("module", "mod-languages: table `language_teacher` is missing or empty, nothing will be taught. "
                               "Apply sql/05_language_teachers.sql.");
            return;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = fields[0].Get<uint32>();
            uint32 const spell = fields[1].Get<uint32>();

            if (!sSpellMgr->GetSpellInfo(spell))
            {
                LOG_ERROR("sql.sql", "mod-languages: `language_teacher` row for creature {} has unknown spell {}, skipped.",
                    entry, spell);
                continue;
            }

            LanguageOffer offer;
            offer.spell = spell;
            offer.cost = fields[2].Get<uint32>();
            offer.requiredFaction = fields[3].Get<uint32>();
            offer.requiredRank = std::min<uint8>(fields[4].Get<uint8>(), MAX_REPUTATION_RANK - 1);
            offer.name = fields[5].Get<std::string>();

            if (offer.requiredFaction && !sFactionStore.LookupEntry(offer.requiredFaction))
            {
                LOG_ERROR("sql.sql", "mod-languages: `language_teacher` row for creature {} has unknown faction {}, requirement dropped.",
                    entry, offer.requiredFaction);
                offer.requiredFaction = 0;
            }

            teachers[entry].push_back(offer);
            ++count;
        } while (result->NextRow());

        LOG_INFO("module", "mod-languages: loaded {} language offer(s) for {} teacher(s)", count, teachers.size());
    }

    // Spell names read "Language Common" and, for Kalimag, "Language Old
    // Tongue (NYI)" - Blizzard never finished the player facing spell even
    // though the client knows the language perfectly well. Tidy that up, and
    // let the database override the label outright.
    std::string LanguageName(LanguageOffer const& offer)
    {
        if (!offer.name.empty())
            return offer.name;

        std::string name;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(offer.spell))
            if (char const* spellName = info->SpellName[sWorld->GetDefaultDbcLocale()])
                name = spellName;

        if (name.empty())
            return Acore::StringFormat("spell {}", offer.spell);

        if (name.rfind("Language ", 0) == 0)
            name.erase(0, 9);

        if (std::size_t const nyi = name.find(" (NYI)"); nyi != std::string::npos)
            name.erase(nyi);

        return name;
    }

    char const* RankName(uint8 rank)
    {
        static constexpr char const* names[MAX_REPUTATION_RANK] =
        {
            "Hated", "Hostile", "Unfriendly", "Neutral", "Friendly", "Honored", "Revered", "Exalted"
        };

        return rank < MAX_REPUTATION_RANK ? names[rank] : "Unknown";
    }

    std::string FactionName(uint32 faction)
    {
        if (FactionEntry const* entry = sFactionStore.LookupEntry(faction))
            if (char const* name = entry->name[sWorld->GetDefaultDbcLocale()])
                return name;

        return Acore::StringFormat("faction {}", faction);
    }

    bool MeetsRequirement(Player* player, LanguageOffer const& offer)
    {
        if (!cfg.RequireReputation || !offer.requiredFaction)
            return true;

        return uint8(player->GetReputationRank(offer.requiredFaction)) >= offer.requiredRank;
    }

    std::string MoneyText(uint32 copper)
    {
        uint32 const gold = copper / GOLD;
        uint32 const silver = (copper % GOLD) / SILVER;
        uint32 const rest = copper % SILVER;

        if (gold && !silver && !rest)
            return Acore::StringFormat("{} gold", gold);

        return Acore::StringFormat("{}g {}s {}c", gold, silver, rest);
    }
}

class Languages_WorldScript : public WorldScript
{
public:
    Languages_WorldScript() : WorldScript("Languages_WorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // On a config reload everything else is already up; at startup the
        // spell and faction stores do not exist yet, so the table is read from
        // OnStartup instead.
        if (reload)
            LoadTeachers();
    }

    void OnStartup() override
    {
        LoadTeachers();
    }
};

class Languages_CreatureScript : public AllCreatureScript
{
public:
    Languages_CreatureScript() : AllCreatureScript("Languages_CreatureScript") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!cfg.Enable || !player || !creature)
            return false;

        auto const teacher = teachers.find(creature->GetEntry());
        if (teacher == teachers.end())
            return false;

        // Only the languages this character is missing are worth showing.
        std::vector<LanguageOffer> available;
        for (LanguageOffer const& offer : teacher->second)
            if (!player->HasSpell(offer.spell))
                available.push_back(offer);

        if (available.empty())
            return false;   // nothing to add, let the normal gossip happen

        // Rebuild what the player would have seen, then add to it.
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        for (LanguageOffer const& offer : available)
        {
            // The requirement is shown rather than hidden, so a character can
            // see what it is working towards.
            if (!MeetsRequirement(player, offer))
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    Acore::StringFormat("Teach me to speak {} (requires {} with {}).",
                        LanguageName(offer), RankName(offer.requiredRank), FactionName(offer.requiredFaction)),
                    SENDER_LANGUAGE, offer.spell);
                continue;
            }

            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                Acore::StringFormat("Teach me to speak {} ({}).", LanguageName(offer), MoneyText(offer.cost)),
                SENDER_LANGUAGE, offer.spell,
                Acore::StringFormat("Learn {} for {}?", LanguageName(offer), MoneyText(offer.cost)),
                offer.cost, false);
        }

        player->SendPreparedGossip(creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!cfg.Enable || sender != SENDER_LANGUAGE || !player || !creature)
            return false;

        auto const teacher = teachers.find(creature->GetEntry());
        if (teacher == teachers.end())
            return false;

        LanguageOffer const* offer = nullptr;
        for (LanguageOffer const& candidate : teacher->second)
            if (candidate.spell == action)
            {
                offer = &candidate;
                break;
            }

        if (!offer)
            return false;

        ChatHandler handler(player->GetSession());
        CloseGossipMenuFor(player);

        if (player->HasSpell(offer->spell))
            return true;

        if (!MeetsRequirement(player, *offer))
        {
            handler.PSendSysMessage("{} teaches {} only to those {} with {} - you are {}.",
                creature->GetName(), LanguageName(*offer), RankName(offer->requiredRank),
                FactionName(offer->requiredFaction),
                RankName(uint8(player->GetReputationRank(offer->requiredFaction))));
            return true;
        }

        if (!player->HasEnoughMoney(offer->cost))
        {
            handler.PSendSysMessage("You need {} to learn {}.", MoneyText(offer->cost), LanguageName(*offer));
            return true;
        }

        player->ModifyMoney(-int32(offer->cost));
        player->learnSpell(offer->spell);

        if (cfg.Announce)
            handler.PSendSysMessage("{} has taught you to speak {}.", creature->GetName(), LanguageName(*offer));

        LOG_DEBUG("module", "mod-languages: {} learned {} from {} for {}",
            player->GetName(), LanguageName(*offer), creature->GetName(), offer->cost);

        return true;
    }
};

class Languages_CommandScript : public CommandScript
{
public:
    Languages_CommandScript() : CommandScript("Languages_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable languageCommandTable =
        {
            { "list",   HandleLanguageListCommand,   SEC_GAMEMASTER, Console::Yes },
            { "reload", HandleLanguageReloadCommand, SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "language", languageCommandTable }
        };

        return commandTable;
    }

    static bool HandleLanguageListCommand(ChatHandler* handler)
    {
        if (teachers.empty())
        {
            handler->PSendSysMessage("No language teachers are loaded.");
            return true;
        }

        for (auto const& [entry, offers] : teachers)
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
            for (LanguageOffer const& offer : offers)
                handler->PSendSysMessage("  {} ({}) teaches {} for {}{}",
                    info ? info->Name : "<unknown creature>", entry,
                    LanguageName(offer), MoneyText(offer.cost),
                    offer.requiredFaction
                        ? Acore::StringFormat(", needs {} with {}", RankName(offer.requiredRank), FactionName(offer.requiredFaction))
                        : std::string());
        }

        return true;
    }

    static bool HandleLanguageReloadCommand(ChatHandler* handler)
    {
        LoadTeachers();
        handler->PSendSysMessage("Reloaded language teachers: {} teacher(s).", teachers.size());
        return true;
    }
};

void AddLanguagesScripts()
{
    new Languages_WorldScript();
    new Languages_CreatureScript();
    new Languages_CommandScript();
}
