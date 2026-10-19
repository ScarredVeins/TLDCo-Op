// Handing an item to a friend (net/handover.h, milestone 13): which clips, the shared timeline from when the offer and
// the acceptance were heard, where the two stand, and the ledger. Exit code 0 when every check holds.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "net/handover.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

net::Handover make(uint8_t giver, uint8_t receiver, uint32_t count, double at) {
    net::Handover h;
    h.id = net::handover_id(giver, count);
    h.giver = giver;
    h.receiver = receiver;
    h.item = net::HandItem::Food;
    h.gear = "GEAR_CoffeeTin";
    h.offered_ms = at;
    return h;
}

}  // namespace

int main() {
    using net::HandItem;
    {  // Clips and grips.
        expect("food is food whatever its name", net::hand_item_for("GEAR_Stick", true) == HandItem::Food);
        expect("a stick, a firelog and reclaimed wood are wood",
               net::hand_item_for("GEAR_Stick", false) == HandItem::Wood &&
                   net::hand_item_for("GEAR_Firelog", false) == HandItem::Wood &&
                   net::hand_item_for("GEAR_ReclaimedWoodB", false) == HandItem::Wood);
        expect("a hatchet is a tool", net::hand_item_for("GEAR_Hatchet", false) == HandItem::Tool);
        expect("the give clips", !std::strcmp(net::give_clip(HandItem::Food), "give_food") &&
                                     !std::strcmp(net::give_clip(HandItem::Wood), "give_wood") &&
                                     !std::strcmp(net::give_clip(HandItem::Tool), "give"));
        expect("the receive clip matches the giver's rig",
               !std::strcmp(net::receive_clip(HandItem::Food, true), "receive_food_from_f") &&
                   !std::strcmp(net::receive_clip(HandItem::Wood, false), "receive_wood_from_m") &&
                   !std::strcmp(net::receive_clip(HandItem::Tool, true), "receive_from_f"));
        expect("an unknown wire item is a tool", net::hand_item(7) == HandItem::Tool && net::hand_item(1) == HandItem::Food);
        const net::HandPose a = net::receiver_grip(false, false, HandItem::Tool), b = net::receiver_grip(false, false, HandItem::Wood);
        expect("a tool is received like wood", !std::memcmp(&a, &b, sizeof a));
        const net::HandPose f = net::receiver_grip(true, true, HandItem::Food);
        expect("the female pair's food pose is its own", near(f.pos[0], 0.0556f) && near(f.rot[3], -0.6876f));
        expect("pair distances", near(net::pair_distance(false, false), 0.88f) && near(net::pair_distance(false, true), 0.92f) &&
                                     near(net::pair_distance(true, false), 0.84f) && near(net::pair_distance(true, true), 0.88f));
    }
    {  // Where they stand.
        const net::Spot s = net::handover_spot(2, 0, 0, 0, 0.88f, false);
        expect("towards us: all the way, facing us (-x, yaw -90)", near(s.x, 0.88f) && near(s.z, 0) && near(s.yaw, -90));
        const net::Spot m = net::handover_spot(0, 3, 0, 1, 0.88f, true);
        expect("both drawn: half the way each, about the middle", near(m.x, 0) && near(m.z, 2.44f) && (near(m.yaw, 180) || near(m.yaw, -180)));
        const net::Spot same = net::handover_spot(1, 1, 1, 1, 0.9f, false);
        expect("on the same spot: still the distance apart", near(std::hypot(same.x - 1, same.z - 1), 0.9f));
    }
    {  // Looking at a friend.
        const float eye[3] = {0, 1.7f, 0}, ahead[3] = {0, 0, 1};
        expect("straight at a friend 2 m ahead: hit about 1.6 m along",
               near(net::ray_hits_body(eye, ahead, 0, 0, 2, 0.4f, 1.8f, 2.5f), 1.6f));
        expect("a friend 0.5 m to the side of the line: missed", net::ray_hits_body(eye, ahead, 0.5f, 0, 2, 0.4f, 1.8f, 2.5f) < 0);
        expect("too far", net::ray_hits_body(eye, ahead, 0, 0, 4, 0.4f, 1.8f, 2.5f) < 0);
        expect("behind us", net::ray_hits_body(eye, ahead, 0, 0, -2, 0.4f, 1.8f, 2.5f) < 0);
        const float down[3] = {0, -0.6f, 0.8f};
        expect("looking down at their feet: hit", net::ray_hits_body(eye, down, 0, 0, 1.8f, 0.4f, 1.8f, 2.5f) > 0);
        const float up[3] = {0, 0.6f, 0.8f};
        expect("looking over their head: missed", net::ray_hits_body(eye, up, 0, 0, 1.8f, 0.4f, 1.8f, 2.5f) < 0);
        const float sky[3] = {0, 1, 0};
        expect("straight up from inside them: through the top", near(net::ray_hits_body(eye, sky, 0, 0, 0, 0.4f, 1.8f, 2.5f), 0));
    }
    {  // The timeline: an offer waits at the hold.
        auto v = net::handover_view(1000, 0, 0, false, 2000);
        expect("an offer: the give clip runs, the item in the giver's hand",
               near(v.giver_t, 1) && v.receiver_t < 0 && v.item_shown && !v.in_receiver && !v.fading && !v.over);
        v = net::handover_view(1000, 0, 0, false, 20000);
        expect("an offer not taken waits at the hold", near(v.giver_t, net::kHoldAt) && v.receiver_t < 0);
        v = net::handover_view(1000, 0, 21000, false, 21100);
        expect("taken back: fades from the hold", v.fading && near(v.giver_t, net::kHoldAt) && !v.over);
        v = net::handover_view(1000, 0, 21000, false, 21400);
        expect("taken back: over after the fade", v.over);
        v = net::handover_view(1000, 0, 1500, false, 1600);
        expect("taken back early: fades from where it was", v.fading && near(v.giver_t, 0.5f));
    }
    {  // Accepted while waiting: the receiver reaches, then both run on together.
        const double offered = 0, accepted = 10000;
        auto v = net::handover_view(offered, accepted, 0, false, accepted);
        expect("accepted: the receive clip starts at the reach", near(v.receiver_t, net::kReachFrom) && near(v.giver_t, net::kHoldAt));
        const double go = accepted + (net::kHoldAt - net::kReachFrom) * 1000.0;
        v = net::handover_view(offered, accepted, 0, false, go - 100);
        expect("reaching: the giver still holds", near(v.giver_t, net::kHoldAt) && near(v.receiver_t, net::kHoldAt - 0.1f) && !v.in_receiver);
        v = net::handover_view(offered, accepted, 0, false, go + 50);
        expect("together past the hold, not yet transferred", near(v.giver_t, net::kHoldAt + 0.05f) &&
                                                                 near(v.receiver_t, v.giver_t) && !v.in_receiver);
        v = net::handover_view(offered, accepted, go + 200, true, go + 200);
        expect("past the transfer: in the receiver's hand", v.in_receiver && v.item_shown);
        v = net::handover_view(offered, accepted, go + 200, true, go + (net::kHandoverLength - net::kHoldAt + 0.1f) * 1000);
        expect("past the end: the giver is out, the receiver holds its last pose",
               v.giver_t < 0 && near(v.receiver_t, net::kHandoverLength) && v.item_shown && !v.fading);
        v = net::handover_view(offered, accepted, go + 200, true,
                               go + (net::kHandoverLength - net::kHoldAt + net::kCarrySeconds + 0.1f) * 1000);
        expect("after the carry: stowed, fading", !v.item_shown && v.fading && !v.over);
        v = net::handover_view(offered, accepted, go + 200, true,
                               go + (net::kHandoverLength - net::kHoldAt + net::kCarrySeconds + net::kHandoverFade + 0.01f) * 1000);
        expect("then over", v.over);
    }
    {  // Accepted before the giver reached the hold: both go on, and run on together once both are there.
        auto v = net::handover_view(0, 500, 0, false, 1500);
        expect("an early acceptance: both clips run", near(v.giver_t, 1.5f) && near(v.receiver_t, net::kReachFrom + 1) && !v.in_receiver);
        const double go = 500 + (net::kHoldAt - net::kReachFrom) * 1000.0;
        v = net::handover_view(0, 500, 0, false, go - 100);
        expect("the giver waits at the hold for the receiver's reach",
               near(v.giver_t, net::kHoldAt) && near(v.receiver_t, net::kHoldAt - 0.1f));
        v = net::handover_view(0, 500, 0, false, go + 100);
        expect("then together", near(v.giver_t, net::kHoldAt + 0.1f) && near(v.receiver_t, v.giver_t));
    }
    {  // It couldn't be given after the acceptance (the giver no longer had it).
        const double go = 10000 + (net::kHoldAt - net::kReachFrom) * 1000.0;
        auto v = net::handover_view(0, 10000, 10500, false, 10600);
        expect("failed while reaching: fades from where both were", v.fading && near(v.giver_t, net::kHoldAt) &&
                                                                        near(v.receiver_t, net::kReachFrom + 0.5f) && !v.in_receiver);
        v = net::handover_view(0, 10000, 10500, false, 10900);
        expect("then over", v.over);
        v = net::handover_view(0, 10000, go + 500, false, go + 600);
        expect("a failure heard after the transfer plays on", !v.fading && v.in_receiver);
    }
    {  // The ledger.
        net::Handovers l;
        expect("an offer", l.offer(make(1, 0, 1, 0)));
        expect("the same one twice is refused", !l.offer(make(1, 0, 1, 0)));
        expect("to oneself is refused", !l.offer(make(2, 2, 1, 0)));
        expect("the giver can't offer twice at once", !l.offer(make(1, 2, 2, 0)));
        expect("nobody can offer to someone in one", !l.offer(make(2, 0, 1, 0)));
        expect("two others can", l.offer(make(2, 3, 1, 0)));
        const uint32_t id = net::handover_id(1, 1);
        expect("the id names the giver", net::handover_giver(id) == 1);
        expect("open for both", l.open_of(1) && l.open_of(0) && l.open_of(1)->id == id && !l.open_of(4));
        expect("not done before it's accepted", !l.end(id, 100, true) && !l.take(id));
        expect("accepted", l.accept(id, 1000));
        expect("not twice", !l.accept(id, 1100));
        expect("nothing to take until it's done", !l.take(id));
        expect("done", l.end(id, 2000, true));
        expect("not over twice", !l.end(id, 2100, false));
        expect("the item is taken once", l.take(id) && !l.take(id));
        expect("no longer open", !l.open_of(1) && !l.open_of(0));
        expect("its body still shows it", l.shown_for(0, 2100) && l.shown_for(0, 2100)->id == id);
        expect("and not long after", !l.shown_for(0, 2000 + 10000));
        expect("a new offer once it's over", l.offer(make(1, 0, 2, 3000)));
        expect("a stale offer", l.stale(1, 3000 + net::kOfferTimeoutMs + 1, net::kOfferTimeoutMs).size() == 1 &&
                                    l.stale(1, 3000 + 10, net::kOfferTimeoutMs).empty());
        expect("a player leaving ends theirs", l.end_all_of(3, 5000).size() == 1 && !l.open_of(2));
        const uint32_t taken_back = net::handover_id(1, 2);
        expect("taken back, nothing to take", l.end(taken_back, 6000, false) && !l.take(taken_back));
        l.prune(6000 + 60001);
        expect("pruned once over a while", !l.find(id) && !l.find(taken_back) && l.all().empty());
        expect("a late message about one is unknown", !l.accept(id, 70000) && !l.end(id, 70000, true));
    }
    std::printf("%s\n", g_failed ? "FAILED" : "all handover checks pass");
    return g_failed ? 1 : 0;
}
