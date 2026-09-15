#include "ItemEligibility.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include "ItemTemplate.h"

namespace
{
    // Matched as WHOLE WORDS that are also ALL-UPPERCASE in the original name.
    // Both conditions matter:
    //
    // - Whole-word, because substring matching is actively wrong here: "test"
    //   appears inside "Greatest", "ph" inside hundreds of ordinary words.
    //
    // - All-uppercase, because whole-word alone still eats real items. "Test of
    //   Faith" is a legitimate item name containing the standalone word "Test".
    //   Blizzard's internal rows are consistently SHOUTED ("AHNQIRAJ TEST ITEM A
    //   CLOTH BELT", "[PH] ...", "QA ..."), while real item names use title case,
    //   so casing is what actually separates the two.
    //
    // Erring toward keeping a junk row visible over hiding a real tradable one,
    // per the header's stated bar.
    constexpr std::array<std::string_view, 7> kJunkWords = {
        "test", "qa", "deprecated", "unused", "placeholder", "ph", "debug",
    };

    char LowerAscii(char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // A "word" is a run of alphanumerics; every other byte is a separator, so
    // "[PH]" tokenises to "ph" and "TEST," to "test" -- how these names actually
    // appear. A word qualifies only if it had no lowercase letters in the
    // original, and contains at least one letter (so a bare number can't match).
    bool ContainsJunkWord(std::string const& name)
    {
        std::string word;
        bool sawLower = false;
        bool sawAlpha = false;
        word.reserve(16);

        auto flush = [&]() -> bool
        {
            bool junk = !word.empty() && !sawLower && sawAlpha &&
                std::find(kJunkWords.begin(), kJunkWords.end(), word) != kJunkWords.end();
            word.clear();
            sawLower = false;
            sawAlpha = false;
            return junk;
        };

        for (char c : name)
        {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                if (std::isalpha(uc))
                {
                    sawAlpha = true;
                    if (std::islower(uc))
                    {
                        sawLower = true;
                    }
                }
                word.push_back(LowerAscii(c));
            }
            else if (flush())
            {
                return true;
            }
        }
        return flush();
    }

    // Item classes that are never player-auctionable regardless of their data.
    // Quest items and keys are soulbound-by-nature; money/permanent are internal
    // container classes that never exist as a real inventory item.
    bool IsNonAuctionableClass(uint32 itemClass)
    {
        return itemClass == ITEM_CLASS_QUEST || itemClass == ITEM_CLASS_KEY ||
               itemClass == ITEM_CLASS_MONEY || itemClass == ITEM_CLASS_PERMANENT;
    }
}

namespace ItemEligibility
{
    bool IsAuctionableItem(ItemTemplate const& proto)
    {
        // An unnamed row is never a real item -- and its name is what the addon
        // would display, so there would be nothing to show even if it were.
        if (proto.Name1.empty())
        {
            return false;
        }

        // Bind on Pickup can only ever reach a vendor, never the auction house --
        // there is no price to suggest for it. This is also a real stability
        // fix, not just correctness: BoP items were crashing the server on
        // lookup (reported directly), same failure class as the junk/TEST rows
        // this file already exists to keep out of every addon-facing path.
        if (proto.Bonding == BIND_WHEN_PICKED_UP)
        {
            return false;
        }

        if (IsNonAuctionableClass(proto.Class))
        {
            return false;
        }

        if (ContainsJunkWord(proto.Name1))
        {
            return false;
        }

        // Economically inert AND level-less. Either alone is legitimate on its
        // own (quest rewards have no vendor price; many trade goods have no
        // required level), so both must hold before treating a row as internal
        // junk -- this is the conservative bar described in the header.
        bool hasAnyValue = proto.BuyPrice > 0 || proto.SellPrice > 0;
        bool hasAnyLevel = proto.ItemLevel > 0 || proto.RequiredLevel > 0;
        if (!hasAnyValue && !hasAnyLevel)
        {
            return false;
        }

        return true;
    }
}
