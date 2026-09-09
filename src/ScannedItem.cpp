#include "ScannedItem.h"
#include <cstdint>
#include <initializer_list>
#include <optional>
#include "ASParse.h"
#include "Tokenize.h"

namespace
{
    // First strictly-positive value in `values`, else `fallback`. Encodes the
    // getters' shared "prefer the outlier-trimmed stat, then fall back through the
    // raw ones" rule in one place so the fallback order stays reviewable.
    uint32 FirstPositive(std::initializer_list<uint32> values, uint32 fallback)
    {
        for (uint32 v : values)
        {
            if (v > 0)
            {
                return v;
            }
        }
        return fallback;
    }
}

bool ParseStatBlock(std::vector<std::string_view> const& fields, size_t offset, StatBlock& out)
{
    if (offset + ScannedItem::kStatsPerBlock > fields.size())
    {
        return false;
    }

    // Order matches StatBlock's declaration and compile-data.cpp's emitStats().
    uint32* const cols[ScannedItem::kStatsPerBlock] = {
        &out.low, &out.high, &out.mean, &out.median, &out.mode,
        &out.q1, &out.q3,
        &out.adjLow, &out.adjHigh, &out.adjMean, &out.adjMedian, &out.adjMode,
    };
    for (size_t i = 0; i < ScannedItem::kStatsPerBlock; ++i)
    {
        if (!ASParse::ClampedU32(fields[offset + i], *cols[i]))
        {
            return false;
        }
    }
    return true;
}

std::optional<ScannedItem> ScannedItem::TryParse(std::string_view dataLine)
{
    std::vector<std::string_view> f = Acore::Tokenize(dataLine, ':', false);
    if (f.size() != kRowFields)
    {
        return std::nullopt;
    }

    ScannedItem item;

    uint32 factionNum = 0;
    if (!ASParse::Integer(f[0], factionNum) || factionNum > UINT8_MAX)
    {
        return std::nullopt;
    }
    item.factionNum = static_cast<uint8>(factionNum);

    if (!ASParse::Integer(f[1], item.itemID) || !ASParse::Integer(f[2], item.suffixID) ||
        !ASParse::Integer(f[3], item.sampleCount))
    {
        return std::nullopt;
    }

    StatBlock* const blocks[kStatBlockCount] = {&item.price, &item.stack, &item.listing};
    for (size_t b = 0; b < kStatBlockCount; ++b)
    {
        if (!ParseStatBlock(f, kIdentityFields + b * kStatsPerBlock, *blocks[b]))
        {
            return std::nullopt;
        }
    }

    if (!ASParse::ClampedU32(f[kRowFields - 1], item.listingSnapshotCount))
    {
        return std::nullopt;
    }

    return item;
}

uint32 ScannedItem::GetMarketPrice() const
{
    return FirstPositive({price.adjMedian, price.adjMean, price.median}, price.mean);
}

uint32 ScannedItem::GetListLow() const { return FirstPositive({price.adjLow}, GetMarketPrice()); }
uint32 ScannedItem::GetListHigh() const { return FirstPositive({price.adjHigh}, GetMarketPrice()); }

uint32 ScannedItem::GetBuyCeiling() const
{
    uint32 market = GetMarketPrice();
    return price.q3 > market ? price.q3 : market;
}

uint32 ScannedItem::GetTypicalStackSize() const
{
    return FirstPositive({stack.adjMode, stack.adjMedian, stack.mode, stack.median}, 1);
}

uint32 ScannedItem::GetStackLow() const { return FirstPositive({stack.adjLow}, 1); }
uint32 ScannedItem::GetStackHigh() const { return FirstPositive({stack.adjHigh}, GetTypicalStackSize()); }

uint32 ScannedItem::GetTypicalListingCount() const
{
    return FirstPositive({listing.adjMedian, listing.adjMean, listing.median, listing.mean}, 1);
}

ScannedItem ScannedItem::FromOverride(
    uint8 factionNum,
    uint32 itemID,
    uint32 marketPrice,
    uint32 listLow,
    uint32 listHigh,
    uint32 typicalStack,
    uint32 stackLow,
    uint32 stackHigh,
    bool markAsOverride)
{
    ScannedItem item;
    item.isOverride = markAsOverride;
    item.factionNum = factionNum;
    item.itemID = itemID;
    item.suffixID = 0;
    item.sampleCount = 1;
    item.listingSnapshotCount = 1;

    // Price block: every getter reads adjMedian/adjLow/adjHigh/q3 first, with
    // mean/median as fallback -- fill all of them from the same market price so
    // no path falls through to 0.
    item.price.mean = item.price.median = item.price.mode = marketPrice;
    item.price.adjMean = item.price.adjMedian = item.price.adjMode = marketPrice;
    item.price.low = item.price.q1 = item.price.adjLow = listLow;
    item.price.high = item.price.q3 = item.price.adjHigh = listHigh;

    // Stack block: same pattern, keyed off adjMode (typical) / adjLow / adjHigh.
    item.stack.mean = item.stack.median = item.stack.mode = typicalStack;
    item.stack.adjMean = item.stack.adjMedian = item.stack.adjMode = typicalStack;
    item.stack.low = item.stack.adjLow = stackLow;
    item.stack.high = item.stack.adjHigh = stackHigh;

    // Listing-count block: no real snapshot data exists for a manually-priced
    // item, so assume it typically carries one concurrent auction -- enough to
    // let it be selected without artificially inflating its listing weight.
    item.listing.mean = item.listing.median = item.listing.mode = 1;
    item.listing.adjMean = item.listing.adjMedian = item.listing.adjMode = 1;
    item.listing.low = item.listing.high = item.listing.q1 = item.listing.q3 = 1;
    item.listing.adjLow = item.listing.adjHigh = 1;

    return item;
}
