#pragma once
// Test helpers, so a test can be set up without walking there. Write one command to <game>\TLDCoop\command.txt;
// the mod runs it within half a second (in game), logs the result to coop.log and deletes the file.
//   at MS COMMAND  - run COMMAND when the PC's clock reaches MS (ms since 1970, UTC): two copies doing something at
//                    the same moment (e.g. both picking up one item)
//   doors          - this scene's doors (LoadScene): index, where each leads, inside / outside, distance
//   goto_door [N]  - stand 3 m from door N, facing it (default: the nearest door into a building)
//   enter [N]      - go through door N (default: the nearest door into a building), as using it does
//   info           - difficulty, scene and how many animals of each kind are around
//   animals        - every animal (BaseAi): kind, AI mode, health, distance
//   spawn KIND [N] - N (default 1) real, AI-driven animals of KIND (wolf, bear, stag, rabbit, moose, cougar) about
//                    25 m ahead, on walkable ground, facing us; made by this region's own spawn area for that kind
//   goto_animal [KIND] - stand 25 m from the nearest living animal (of KIND), facing it
//   goto X Y Z     - stand at X Y Z (world coordinates, as the log prints them)
//   console NAME   - one of TLD's own debug console commands (CONSOLE_NAME): god, invulnerable, ghost, fly...
//   time HH:MM     - set the clock (on the client, the host's clock takes over again within a minute)
//   run LINE       - a line for TLD's own console (uConsole.RunCommand). It does nothing in the 2.55 release build
//                    (the console's commands aren't registered there): use console NAME, give or equip
//   taps N         - in a struggle, N taps a second (WolfTap in a wolf fight) and the weapon picker answered
//   fight_friend [KIND] / attack_friend [KIND] - host: the nearest animal jumps / goes for the other player
//   attack_host [KIND] - host: the animal nearest the host goes for the host (TLD's own player, to compare)
//   equip GEAR [N] - into the pack and into our hands (GEAR_Rifle, GEAR_Revolver, GEAR_Bow, GEAR_FlareGun...); a gun
//                    gets N rounds (default 5)
//   give GEAR [N]  - N of an item into the pack (GEAR_Arrow, GEAR_FlareGunAmmoSingle, GEAR_Stone...)
//   approach [M]   - friend: stand M m (default 15) from the nearest of the host's animals
//   shoot [COLLIDER] - friend: aim at that hit collider of the nearest host animal (default: its torso) and fire
//                    the weapon in our hands through TLD's own vp_FPSShooter.Fire
//   near X Y Z [M] - stand M m (default 4) from that point, facing it
//   look X Y Z     - point the camera at that point
//   freeze [S] [ID] - host: animal #ID (default: the one nearest the other player) stands still for S seconds
//                    (default 8): its AI and Animator paused
//   kill [KIND]    - host: the animal nearest the other player dies (default wolf)
//   harvest KG [HIDE] [GUT] - take that much from the nearest carcass through TLD's harvest panel
//   quarter        - quarter the nearest carcass (TLD's harvest panel)
//   carcasses      - the carcasses around: meat, hide, gut, condition
//   items [M]      - loose items within M m (default 10): id, name, position, shared or not
//   pack           - what's in our pack, with persistent ids
//   pickup [NAME]  - pick up the nearest loose item (whose name contains NAME) within 6 m, as TLD does
//   drop NAME [N]  - drop N (default 1) of the item from our pack whose name contains NAME (GearItem.Drop)
//   putback        - leave TLD's inspect screen ("Take it / Leave it") the way its Leave it button does
//   takeit         - the same screen's Take it button (e.g. an item found in a container's search)
//   inspect [NAME] - pick up the nearest loose item (whose name contains NAME) within 6 m into TLD's inspect screen
//                    (Take it / Leave it: PlayerManager.EnterInspectGearMode), as picking up a new kind of item does
//   coin fair|first|last - host: the coin toss for two takes of one item at the same moment: fair (default), the
//                    earlier take (or hold) keeps it, or the newcomer gets it
//   snare [set|catch|break|roll] - the nearest snare lying within 6 m: its state; set it as placing it does
//                    (SnareItem.OnSnarePlaced), make it catch a rabbit as a lucky roll does (SetState + SpawnDeadRabbitOnSnare),
//                    break it, or roll for a catch the way TLD does (DoRoll: a friend skips it where the host is)
//   record [NAME]  - the nearest loose item's (whose name contains NAME) save record, as ItemPut sends it
//   cooking [H|take|put NAME|boil L|melt L] - pots and food cooking on fires within 6 m: what, how far along; H:
//                    they cook H game hours more (here only, as if the time had passed); take: take the first one's
//                    food or water as a player does; put / boil / melt: food from the pack (StartCooking), L litres of
//                    water (StartBoilingWater, taken from the pack) or snow (StartMeltingSnow) into the first real pot
//   containers [M] - containers within M m (default 10): id, searched or not, what's in them, a hash of their save text
//   open [NAME]    - open the nearest container (whose name contains NAME) within 6 m: its panel
//   search [NAME]  - the same the way holding the interact button does it: the search (Container.BeginContainerOpen),
//                    then 1.5 s later what it found, one item at a time (takeit / putback), or the panel
//   close          - close the container panel
//   take NAME      - move an item whose name contains NAME from the open (or nearest) container into the pack
//   put NAME       - move an item whose name contains NAME from the pack into the open (or nearest) container
//   objects [M]    - breakdown objects, plants, locks and safes within M m (default 15): id, broken down / harvested /
//                    locked or not
//   breakdown [NAME] - break down the nearest one (whose name contains NAME) within 6 m, as finishing TLD's break down
//                    panel does (the yield goes into the pack)
//   plant [NAME]   - harvest the nearest plant (whose name contains NAME) within 6 m, as TLD's interaction does
//   force [NAME]   - force the nearest locked lock (whose name contains NAME) within 6 m, as finishing TLD's prybar
//                    force does (Lock.FinishForceLock(true))
//   crack [NAME]   - crack the nearest locked safe (whose name contains NAME) within 6 m (SafeCracking.UnlockSafe)
//   die [CAUSE]    - the player dies, TLD's own way (Condition.KillPlayer; CAUSE a DamageSource number, 0 TLD's
//                    pick); god mode doesn't stop it, and a test copy doesn't delete its save
//   afterlife [left|right|back|confirm] - TLD's screen after a death: its stage, or a button pressed (left: CHEAT
//                    DEATH, NEXT, the short penalty, the close respawn; right: END GAME, the permanent penalty, the
//                    far one; confirm: CONFIRM on its "are you sure")
//   respawn [BODY] - co-op, dead: RESPAWN on co-op's screen after a death, as BODY (default: the one it shows)
//   survivor [+1|-1] - co-op, dead: the screen's next (or previous) survivor
//   bodies         - the players' bodies here (whose, where, what they hold) and whether we're dead
//   lives N        - the experience mode's Cheat Death lives in this game (0: as a mode without Cheat Death)
//   hands [light|out|aim|lower|high|away] - what's in our hands as State sends it (kind, lit, aiming) and its
//                    lights; or that done to it, as TLD's own actions do without their animations: light
//                    (TorchItem/FlareItem.Ignite; a lantern filled and TurnOn; a flashlight charged and TurnOn), out
//                    (ForceExtinguish, TurnOff, TurnOffImmediate), aim / lower (the aim button held / let go, as a
//                    player holds it: a gun raised; it doesn't draw a bow), high (the flashlight's high beam), away
//                    (PlayerManager.UnequipImmediate)
//   held           - what each other player holds as shown here: the copy in their hand, lit or not, its lights,
//                    how far its grip is from the hand
//   hold KEY FIELD VALUES - how a kind is held, tuned live (KEY as `held` prints it, e.g. torch, rifle_aim; FIELD
//                    rh|lh|dir|up x y z, grip G, left M, look|right|leftarm|hangs|flip 0|1, reset, show; or
//                    `hold pitch 1|-1`, or `hold part NAME 0|1`: that part of each copy shown or hidden)
//   needs [on|off] - our hunger, thirst, fatigue, warmth and condition, their rates, the clock and its scale; on:
//                    a test copy in god mode pays its needs from now on (still can't die), off: god mode again
//   lapse [S] [MIN] - TLD's generic progress bar (eating, drinking, first aid use it) for S real seconds (default
//                    10) and MIN game minutes (default 60): an action that speeds the clock up
//   sleep [open|wait|pick C|click C|go|up|wake] - sleeping together: where we and everyone are (in bed, waiting,
//                    votes, what the host would decide); open: the nearest bed's sleep screen (as using it does);
//                    wait: the pass time screen (as the radial's); pick 4|8|12|dawn|dusk on it, as its arrows do;
//                    click C: that row of the vote list, as the mouse does; go: its SLEEP / PASS TIME; up: GET UP /
//                    STOP WAITING (CLOSE); wake: woken in our sleep, as a wolf or the cold does (Rest.EndSleeping
//                    (true)), or our shared passing time stopped (PassTime.End, as Escape does)
//   hud [names|radar|chart on|off] - finding each other: what our HUD shows (each friend's dot or name over their
//                    head or arrow at the edge, the radar and who's on it, friends elsewhere, friends on TLD's map,
//                    charting); or a switch for this session (coop.ini as it was); `hud chart now`: chart where we
//                    stand at once; `hud map open|close|centre`: TLD's map screen (centre: on us); `hud tld hide|show`:
//                    TLD's HUD hidden as its screenshot key does it, then back; `hud radar whole|noclip|normal` and
//                    `hud radar rim|paper SPRITE [R G B A]`: the radar's look, tried out
//   call CLASS METHOD - METHOD (no arguments) on the first active CLASS object of TLD's, as a button would call it
//                    (e.g. `call Panel_Log OnBack`)
//   go NAME [N]    - GameObjects (active or not) whose name contains NAME, the first N (default 20): where, active,
//                    their parents and components
//   decor [take|move|place NAME] - TLD's decorations within 10 m and those we carry (id, where, placed or carried); or
//                    pick up the nearest one (whose name contains NAME, or whose id starts with it) within 6 m
//                    (DecorationItem.OnPickup, as the action picker does), move it half a metre along x, or put one
//                    we carry a metre ahead of us (both as finishing TLD's placement does:
//                    DecorationItem.FinalizePlacementRecursive(true, true))
//   fires [M]      - fires within M m (default 15): id, state, minutes left; and the first one's save record
//   light [NAME]   - light the nearest unlit fire (whose name contains NAME) within 6 m the way TLD's fire panel
//                    does (FireManager.PlayerStartFire, success forced) with a fire starter, tinder and fuel from the pack
//   feed NAME      - put fuel whose name contains NAME from the pack on the nearest burning fire, as the feed panel does
//   cook NAME      - put food or a pot whose name contains NAME from the pack on the nearest burning fire's first
//                    empty cooking slot (GearPlacePoint.DropAndPlaceItem, as the cooking panel does for food without
//                    a pot: TLD puts a dummy pot under it)
//   campfire       - make a campfire 1.5 m ahead of us (FireManager.InstantiateCampFire, put down where we stand), unlit
//   burnout [S]    - the nearest burning fire within 6 m burns only S more game seconds (default 30), in this game
//                    only: what the wind does when it blows a fire out (Fire.MaybeBlowOutFromWind)
//   dismantle [NAME] - dismantle the nearest fire that isn't burning (whose name contains NAME) within 6 m, as TLD's
//                    dismantle action does (FireManager.RemoveFire(fire, true) + Destroy; no yield)
//   join           - (friend) go to the host's scene and stand beside them, as on a first join (even if not the first)
//   cutlink [S]    - drop every packet in and out for S seconds (default 8): the link times out (5 s) and reconnects
//   mode           - the experience mode in place and what a save would write (a friend plays at the host's)
//   save           - save now, as TLD does (GameManager.ForceSaveGame; a nosave copy still skips it)
//   answer let|away - (host, in game) what F9 / F10 do for the first friend asking to join
//   reload         - (test copies) the test save again without a restart: TLD's quit to the main menu, then
//                    SURVIVAL and RESUME as at boot (coop_scenarios --fresh)
// Main thread only.

namespace dev_commands {

bool init();
void on_frame(bool in_game);

}  // namespace dev_commands
