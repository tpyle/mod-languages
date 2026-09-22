# mod-languages

Buy a language from the faction leader who speaks it.

Every racial language in the game is taught by the leader of the race that
speaks it, for gold, so a character can eventually understand everyone -
including the other faction, which matters on a realm where both sides share
a world.

## Configuration (`mod_languages.conf`)

| key | meaning |
| --- | --- |
| `Languages.Enable` | master switch |
| `Languages.Announce` | tell the player at login that this exists |
| `Languages.RequireReputation` | require standing with the teacher's faction |

## Commands

    .language list     the languages this character knows and can buy
    .language reload   re-read the teacher table

## Requirements

The teacher/gossip data in SQL (see the server project's `sql/` directory).
The table is loaded in `OnStartup`, not `OnAfterConfigLoad`: DBC and object
stores do not exist at config-load time, and validating ids there silently
discards every row.
