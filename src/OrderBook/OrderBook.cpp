#include "OrderBook.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

template <Side S>
constexpr bool worse(Price a, Price b) noexcept
{
    if constexpr (S == Side::Buy) return a < b;
    else return a > b;
}

template <Side Taker>
constexpr bool crosses(Price levelPrice, Price limit) noexcept
{
    if constexpr (Taker == Side::Buy) return levelPrice <= limit;
    else return levelPrice >= limit;
}

template <Side S>
constexpr Side opposite() noexcept
{
    return S == Side::Buy ? Side::Sell : Side::Buy;
}

}

OrderBook::OrderBook(std::uint32_t maxOrders, std::size_t expectedLevels)
{
    if (maxOrders == 0 || maxOrders == npos)
        throw std::invalid_argument("maxOrders must be in [1, 2^32 - 2]");

    nodes.resize(maxOrders);
    for (std::uint32_t i = 0; i < maxOrders; ++i)
    {
        nodes[i].id = 0;
        nodes[i].next = i + 1 < maxOrders ? i + 1 : npos;
    }
    freeHead = 0;

    bids.reserve(expectedLevels);
    asks.reserve(expectedLevels);
    tradeBuf.reserve(1024);
}

std::uint32_t OrderBook::allocNode() noexcept
{
    const std::uint32_t idx = freeHead;
    if (idx != npos)
    {
        freeHead = nodes[idx].next;
        // High 32 bits: sequence (unique per order), low 32 bits: pool slot.
        nodes[idx].id = (nextSeq++ << 32) | idx;
    }
    return idx;
}

void OrderBook::freeNode(std::uint32_t idx) noexcept
{
    nodes[idx].id = 0;
    nodes[idx].next = freeHead;
    freeHead = idx;
}

const OrderBook::Node* OrderBook::lookup(OrderId id) const noexcept
{
    const auto slot = static_cast<std::uint32_t>(id);
    if (id == 0 || slot >= nodes.size()) return nullptr;
    const Node& n = nodes[slot];
    return n.id == id ? &n : nullptr;
}

OrderBook::ExecReport OrderBook::submit(Side side, OrderType type, TimeInForce tif, Price price, Quantity qty)
{
    tradeBuf.clear();
    if (qty == 0) return {0, OrderStatus::Rejected, 0, 0};
    return side == Side::Buy ? submitImpl<Side::Buy>(type, tif, price, qty)
                             : submitImpl<Side::Sell>(type, tif, price, qty);
}

template <Side S>
OrderBook::ExecReport OrderBook::submitImpl(OrderType type, TimeInForce tif, Price price, Quantity qty)
{
    if (type == OrderType::Market)
    {
        price = S == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
        if (tif == TimeInForce::GoodTillCancel) tif = TimeInForce::ImmediateOrCancel;
    }

    const std::uint32_t idx = allocNode();
    if (idx == npos) return {0, OrderStatus::Rejected, 0, qty};
    const OrderId id = nodes[idx].id;

    if (tif == TimeInForce::FillOrKill && !canFill<S>(price, qty))
    {
        freeNode(idx);
        return {id, OrderStatus::Cancelled, 0, qty};
    }

    const Quantity remaining = match<S>(id, price, qty);
    const Quantity filled = qty - remaining;

    if (remaining == 0)
    {
        freeNode(idx);
        return {id, OrderStatus::Filled, filled, 0};
    }
    if (tif != TimeInForce::GoodTillCancel)
    {
        freeNode(idx);
        return {id, OrderStatus::Cancelled, filled, remaining};
    }

    Node& n = nodes[idx];
    n.price = price;
    n.qty = remaining;
    n.side = S;
    rest<S>(idx);
    ++liveOrders;
    return {id, filled ? OrderStatus::PartiallyFilled : OrderStatus::New, filled, remaining};
}

template <Side S>
Quantity OrderBook::match(OrderId takerId, Price limit, Quantity qty)
{
    Levels& book = levels<opposite<S>()>();

    while (qty != 0 && !book.empty())
    {
        PriceLevel& lvl = book.back();
        if (!crosses<S>(lvl.price, limit)) break;

        std::uint32_t idx = lvl.head;
        while (qty != 0 && idx != npos)
        {
            Node& maker = nodes[idx];
            const Quantity fill = std::min(qty, maker.qty);
            tradeBuf.push_back({maker.id, takerId, lvl.price, fill, S});

            qty -= fill;
            maker.qty -= fill;
            lvl.qty -= fill;

            const std::uint32_t next = maker.next;
            if (maker.qty == 0)
            {
                freeNode(idx);
                --liveOrders;
                idx = next;
            }
        }

        if (idx == npos)
        {
            book.pop_back();
        }
        else
        {
            lvl.head = idx;
            nodes[idx].prev = npos;
        }
    }
    return qty;
}

template <Side S>
bool OrderBook::canFill(Price limit, Quantity qty) const noexcept
{
    const Levels& book = levels<opposite<S>()>();
    std::uint64_t available = 0;
    for (auto it = book.rbegin(); it != book.rend() && crosses<S>(it->price, limit); ++it)
    {
        available += it->qty;
        if (available >= qty) return true;
    }
    return false;
}

template <Side S>
OrderBook::Levels::const_iterator OrderBook::findLevel(Price price) const noexcept
{
    const Levels& book = levels<S>();
    return std::lower_bound(book.begin(), book.end(), price,
                            [](const PriceLevel& l, Price p) { return worse<S>(l.price, p); });
}

template <Side S>
void OrderBook::rest(std::uint32_t idx)
{
    Levels& book = levels<S>();
    Node& n = nodes[idx];
    n.next = npos;

    const auto pos = book.begin() + (findLevel<S>(n.price) - book.cbegin());
    if (pos != book.end() && pos->price == n.price)
    {
        n.prev = pos->tail;
        nodes[pos->tail].next = idx;
        pos->tail = idx;
        pos->qty += n.qty;
    }
    else
    {
        n.prev = npos;
        book.insert(pos, PriceLevel{n.price, n.qty, idx, idx});
    }
}

template <Side S>
void OrderBook::unlink(std::uint32_t idx) noexcept
{
    Levels& book = levels<S>();
    const Node& n = nodes[idx];
    const auto pos = book.begin() + (findLevel<S>(n.price) - book.cbegin());

    if (n.prev != npos) nodes[n.prev].next = n.next;
    else pos->head = n.next;
    if (n.next != npos) nodes[n.next].prev = n.prev;
    else pos->tail = n.prev;

    pos->qty -= n.qty;
    if (pos->head == npos) book.erase(pos);
}

bool OrderBook::cancel(OrderId id) noexcept
{
    const Node* n = lookup(id);
    if (!n) return false;

    const auto idx = static_cast<std::uint32_t>(id);
    if (n->side == Side::Buy) unlink<Side::Buy>(idx);
    else unlink<Side::Sell>(idx);
    freeNode(idx);
    --liveOrders;
    return true;
}

bool OrderBook::reduce(OrderId id, Quantity newQty) noexcept
{
    const Node* n = lookup(id);
    if (!n || newQty >= n->qty) return false;
    if (newQty == 0) return cancel(id);

    const auto idx = static_cast<std::uint32_t>(id);
    const Quantity delta = n->qty - newQty;
    Levels& book = n->side == Side::Buy ? bids : asks;
    const auto off = n->side == Side::Buy ? findLevel<Side::Buy>(n->price) - bids.cbegin()
                                          : findLevel<Side::Sell>(n->price) - asks.cbegin();
    book[off].qty -= delta;
    nodes[idx].qty = newQty;
    return true;
}

OrderBook::ExecReport OrderBook::replace(OrderId id, Price price, Quantity qty)
{
    const Node* n = lookup(id);
    if (!n)
    {
        tradeBuf.clear();
        return {0, OrderStatus::Rejected, 0, qty};
    }
    const Side side = n->side;
    cancel(id);
    return submit(side, OrderType::Limit, TimeInForce::GoodTillCancel, price, qty);
}

std::optional<Price> OrderBook::bestBid() const noexcept
{
    if (bids.empty()) return std::nullopt;
    return bids.back().price;
}

std::optional<Price> OrderBook::bestAsk() const noexcept
{
    if (asks.empty()) return std::nullopt;
    return asks.back().price;
}

std::uint64_t OrderBook::volumeAt(Side side, Price price) const noexcept
{
    const Levels& book = side == Side::Buy ? bids : asks;
    const auto it = side == Side::Buy ? findLevel<Side::Buy>(price) : findLevel<Side::Sell>(price);
    return it != book.end() && it->price == price ? it->qty : 0;
}

std::vector<OrderBook::DepthLevel> OrderBook::depth(Side side, std::size_t count) const
{
    const Levels& book = side == Side::Buy ? bids : asks;
    std::vector<DepthLevel> out;
    out.reserve(std::min(count, book.size()));
    for (auto it = book.rbegin(); it != book.rend() && out.size() < count; ++it)
        out.push_back({it->price, it->qty});
    return out;
}

std::optional<OrderBook::OrderInfo> OrderBook::find(OrderId id) const noexcept
{
    const Node* n = lookup(id);
    if (!n) return std::nullopt;
    return OrderInfo{n->id, n->side, n->price, n->qty};
}
