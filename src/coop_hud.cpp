#include "coop_hud.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "game_state.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/hud.h"
#include "net_runtime.h"

namespace coop_hud {
namespace {

namespace hud = net::hud;

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr int kHudOff = 6, kHudScreenShot = 4;  // HudDisplayMode: TLD's HUD hidden
constexpr int kPanelDepth = 6;                  // our panel, over NonEssentialHud's own widgets
constexpr int kPivotCenter = 4, kPivotTopRight = 2, kPivotRight = 5, kPivotLeft = 3;  // UIWidget.Pivot
constexpr int kOverflowResizeFreely = 2;                                                // UILabel.Overflow

// Friends in our scene: a small dot over the head, the name beside it only when they're close or we look at them
// (hud::name_wanted); out of sight, a small arrow at the screen's edge. Quiet: small, dim, fading in and out.
constexpr float kHeadAbove = 2.05f;     // the dot this far over a friend's feet: just over the head
constexpr float kBodyMiddle = 1.0f;     // looking at a friend: at this height over their feet
constexpr float kTextAbove = 12;        // the name this many UI units over the dot
constexpr float kEdgeMargin = 0.06f;    // out of sight: the arrow this far inside the screen's edge (share of height)
constexpr float kArrowToText = 18;      // the name this far from the arrow, towards the screen's centre
constexpr int kNameFont = 13, kDistanceFont = 11, kElsewhereFont = 13;
constexpr float kNameGap = 6;           // between a name and its distance, on one line
constexpr int kDotSize = 14, kArrowSize = 26;  // TLD's crosshair dot (crosshair4, 22 there) and arrow_nav3 (30)
constexpr float kDotAlpha = 0.5f, kArrowAlpha = 0.65f, kNameAlpha = 0.8f, kDistanceAlpha = 0.5f;
constexpr float kFadeSeconds = 0.25f;

struct Vector2 {
    float x, y;
};
struct Vector3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};

struct Api {
    // Unity
    Il2CppClass* game_object_class = nullptr;
    const MethodInfo* go_ctor = nullptr;            // GameObject(string)
    const MethodInfo* component_go = nullptr;       // Component.get_gameObject()
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* go_transform = nullptr;
    const MethodInfo* go_set_active = nullptr;
    const MethodInfo* go_active = nullptr;          // GameObject.get_activeInHierarchy()
    const MethodInfo* go_set_layer = nullptr;
    const MethodInfo* add_component = nullptr;      // GameObject.AddComponent(Type)
    const MethodInfo* get_component = nullptr;      // GameObject.GetComponent(Type)
    const MethodInfo* instantiate = nullptr;        // static Object.Instantiate(Object, Transform)
    const MethodInfo* destroy = nullptr;            // static Object.Destroy(Object)
    const MethodInfo* set_name = nullptr;
    const MethodInfo* set_parent = nullptr;         // Transform.SetParent(Transform, bool)
    const MethodInfo* set_position = nullptr;
    const MethodInfo* get_local_position = nullptr;
    const MethodInfo* set_local_position = nullptr;
    const MethodInfo* set_local_euler = nullptr;
    const MethodInfo* set_local_scale = nullptr;
    const MethodInfo* world_to_screen = nullptr;    // Camera.WorldToScreenPoint(Vector3)
    const MethodInfo* screen_to_world = nullptr;    // Camera.ScreenToWorldPoint(Vector3)
    const MethodInfo* screen_width = nullptr;       // static Screen.get_width()
    const MethodInfo* screen_height = nullptr;
    // NGUI
    Il2CppObject* panel_type = nullptr;             // typeof(UIPanel)
    Il2CppObject* anchor_type = nullptr;            // typeof(UIAnchor)
    const MethodInfo* panel_set_depth = nullptr;
    const MethodInfo* ui_camera = nullptr;          // static UICamera.get_mainCamera()
    const MethodInfo* label_set_text = nullptr;
    const MethodInfo* label_set_font_size = nullptr;
    const MethodInfo* label_set_overflow = nullptr;
    const MethodInfo* label_printed = nullptr;      // UILabel.get_printedSize()
    const MethodInfo* widget_set_pivot = nullptr;
    const MethodInfo* widget_set_width = nullptr;
    const MethodInfo* widget_set_height = nullptr;
    const MethodInfo* widget_set_depth = nullptr;
    const MethodInfo* widget_get_color = nullptr;
    const MethodInfo* widget_set_color = nullptr;
    const MethodInfo* sprite_set_name = nullptr;    // UISprite.set_spriteName(string)
    const MethodInfo* sprite_valid = nullptr;       // UISprite.get_isValid(): the atlas has that sprite
    const MethodInfo* widget_width = nullptr;       // UIWidget.get_width()
    const MethodInfo* widget_height = nullptr;
    const MethodInfo* get_local_scale = nullptr;    // Transform.get_localScale()
    const MethodInfo* pixel_adjustment = nullptr;   // static UIRoot.GetPixelSizeAdjustment(GameObject): UI units a pixel
    // the radar
    Il2CppObject* texture_type = nullptr;           // typeof(UITexture)
    const MethodInfo* panel_set_alpha = nullptr;    // UIPanel.set_alpha(float)
    const MethodInfo* panel_set_clipping = nullptr; // UIPanel.set_clipping(UIDrawCall.Clipping)
    const MethodInfo* panel_set_clip_texture = nullptr;  // UIPanel.set_clipTexture(Texture2D)
    const MethodInfo* panel_set_clip_region = nullptr;   // UIPanel.set_baseClipRegion(Vector4)
    const MethodInfo* texture_set_main = nullptr;   // UITexture.set_mainTexture(Texture)
    const MethodInfo* texture_set_uv = nullptr;     // UITexture.set_uvRect(Rect)
    Il2CppClass* texture2d_class = nullptr;
    const MethodInfo* tex_ctor = nullptr;           // Texture2D(int, int, TextureFormat, bool mipChain)
    const MethodInfo* tex_set_pixel = nullptr;      // Texture2D.SetPixel(int, int, Color)
    const MethodInfo* tex_apply = nullptr;          // Texture2D.Apply()
    const MethodInfo* tex_wrap = nullptr;           // Texture.set_wrapMode(TextureWrapMode)
    // TLD's map (Panel_Map) and its fog of war
    Il2CppObject* map_type = nullptr;               // typeof(Panel_Map)
    Il2CppObject* fog_type = nullptr;               // typeof(FogOfWar)
    const MethodInfo* map_to_map = nullptr;         // Panel_Map.WorldPositionToMapPosition(string, Vector3)
    const MethodInfo* map_survey = nullptr;         // Panel_Map.DoNearbyDetailsCheck(float, bool, bool, Vector3, bool)
    const MethodInfo* map_region = nullptr;         // Panel_Map.GetMapNameOfCurrentScene()
    const MethodInfo* map_can_map = nullptr;        // Panel_Map.SceneCanBeMapped(string)
    ptrdiff_t map_elements = -1;                    // m_MapElementsTransform: the map's own icons are under it
    ptrdiff_t map_player_icon = -1;                 // m_PlayerIcon (Transform)
    ptrdiff_t map_survey_radius = -1;               // m_DetailSurveyRadiusMeters: a charcoal survey's
    ptrdiff_t map_objects = -1;                     // m_MapObjects (List<RegionMap>)
    ptrdiff_t map_spots = -1;                       // m_DetailSurveyPositions: region -> List<DetailSurveyPosition>
    ptrdiff_t map_fogs = -1;                        // m_FogOfWar: region -> FogOfWar
    ptrdiff_t map_vistas = -1;                      // m_SurveyedVistaLocations: region -> List<VistaLocationData>
    ptrdiff_t map_regions = -1;                     // m_UnlockedRegionNames (List<string>)
    ptrdiff_t map_region_index = -1;                // m_RegionSelectedIndex: the region the map shows
    const MethodInfo* fog_set_owner = nullptr;      // FogOfWar.SetOwner(Panel_Map)
    const MethodInfo* fog_process = nullptr;        // FogOfWar.ProcessList(spots, vistas, sceneRadius, mapRadius)
    ptrdiff_t fog_texture = -1;                     // FogOfWar.m_RevealedMapTex: what's charted, drawn
    ptrdiff_t fog_camera = -1;                      // FogOfWar.m_Camera: draws it (found in Start)
    const MethodInfo* texture_width = nullptr;      // Texture.get_width() / get_height()
    const MethodInfo* texture_height = nullptr;
    ptrdiff_t region_name = -1;                     // RegionMap.m_RegionName
    ptrdiff_t region_radius = -1;                   // RegionMap.m_RadiusOfScene (Vector3)
    ptrdiff_t spot_x = -1, spot_y = -1;             // DetailSurveyPosition.x / y: its map position, times 3
    const MethodInfo* map_enable = nullptr;         // Panel_Map.Enable(bool): open / close it (dev)
    const MethodInfo* map_centre = nullptr;         // Panel_Map.CenterOnPoint(Vector3 world) (dev)
    const MethodInfo* hud_get_mode = nullptr;       // static HUDManager.get_HudDisplayMode() (dev)
    const MethodInfo* hud_set_mode = nullptr;       // static HUDManager.SetHUDDisplayMode(mode): runtime only (dev)
    const MethodInfo* panel_enabled = nullptr;      // Panel_Base.IsEnabled() (virtual)
    Il2CppObject* sprite_type = nullptr;            // typeof(UISprite)
    // TLD
    Il2CppObject* hud_type = nullptr;               // typeof(Panel_HUD)
    const MethodInfo* load_panel = nullptr;         // static InterfaceManager.LoadPanel(Type)
    const MethodInfo* overlay = nullptr;            // static InterfaceManager.IsOverlayActiveCached()
    const MethodInfo* scene_name = nullptr;         // static InterfaceManager.GetNameForScene(string): "Camp Office"
    const MethodInfo* main_camera = nullptr;        // static GameManager.GetMainCamera()
    ptrdiff_t hud_nonessential = -1;                // Panel_HUD.m_NonEssentialHud (GameObject)
    ptrdiff_t hud_label = -1;                       // m_Label_ObjectName: TLD's hover text
    ptrdiff_t hud_label_back = -1;                  // m_HoverTextBG: its backing (kept hidden by TLD till a hover)
    ptrdiff_t hud_crosshair = -1;                   // m_Sprite_Crosshair: the dot at the screen's centre
    ptrdiff_t hud_text_color = -1;                  // m_HoverTextNormalColor
    ptrdiff_t hud_mode = -1;                        // m_HudDisplayMode
} api;

// --- helpers -------------------------------------------------------------------------------------------------------

template <typename T>
T get(Il2CppObject* o, ptrdiff_t off) {
    return il2::read_field<T>(o, off);
}

bool alive(Il2CppObject* o) { return o && get<void*>(o, kCachedPtr) != nullptr; }
Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
uintptr_t hold(Il2CppObject* o) { return o ? il2::il2cpp_gchandle_new(o, false) : 0; }
void drop(uintptr_t& h) {
    if (h) il2::il2cpp_gchandle_free(h);
    h = 0;
}
Il2CppObject* live(uintptr_t h) {
    Il2CppObject* o = target(h);
    return alive(o) ? o : nullptr;
}

Il2CppObject* go_of(Il2CppObject* component) { return alive(component) ? il2::invoke(api.component_go, component) : nullptr; }
Il2CppObject* transform_of(Il2CppObject* component) {
    return alive(component) ? il2::invoke(api.component_transform, component) : nullptr;
}
Il2CppObject* go_transform(Il2CppObject* go) { return alive(go) ? il2::invoke(api.go_transform, go) : nullptr; }

void set_active(Il2CppObject* go, bool on) {
    if (!alive(go)) return;
    void* args[] = {&on};
    il2::invoke(api.go_set_active, go, args);
}

bool active(Il2CppObject* go) {
    bool on = false;
    return alive(go) && il2::invoke_value(api.go_active, go, on) && on;
}

void set_int(const MethodInfo* setter, Il2CppObject* obj, int v) {
    if (!alive(obj)) return;
    void* args[] = {&v};
    il2::invoke(setter, obj, args);
}

void set_vec(const MethodInfo* setter, Il2CppObject* transform, Vector3 v) {
    if (!alive(transform)) return;
    void* args[] = {&v};
    il2::invoke(setter, transform, args);
}

void set_text(Il2CppObject* label, const std::string& value) {
    if (!alive(label)) return;
    void* args[] = {il2::il2cpp_string_new(value.c_str())};
    il2::invoke(api.label_set_text, label, args);
}

void set_color(Il2CppObject* widget, Color c) {
    if (!alive(widget)) return;
    void* args[] = {&c};
    il2::invoke(api.widget_set_color, widget, args);
}

Color color_of(Il2CppObject* widget) {
    Color c{1, 1, 1, 1};
    if (alive(widget)) il2::invoke_value(api.widget_get_color, widget, c);
    return c;
}

Vector2 printed(Il2CppObject* label) {
    Vector2 v{0, 0};
    if (alive(label)) il2::invoke_value(api.label_printed, label, v);
    return v;
}

void rename(Il2CppObject* obj, const std::string& name) {
    Il2CppObject* go = go_of(obj);
    if (!go) return;
    void* args[] = {il2::il2cpp_string_new(name.c_str())};
    il2::invoke(api.set_name, go, args);
}

// A copy of a widget (and its children) under `parent`, the copy's component of the same type returned.
Il2CppObject* clone_under(Il2CppObject* original, Il2CppObject* parent_transform, const std::string& name) {
    if (!alive(original) || !alive(parent_transform)) return nullptr;
    void* args[] = {original, parent_transform};
    Il2CppObject* copy = il2::invoke(api.instantiate, nullptr, args);
    if (!alive(copy)) return nullptr;
    rename(copy, name);
    set_active(go_of(copy), true);
    set_vec(api.set_local_position, transform_of(copy), {0, 0, 0});
    set_vec(api.set_local_euler, transform_of(copy), {0, 0, 0});
    set_vec(api.set_local_scale, transform_of(copy), {1, 1, 1});
    return copy;
}

Il2CppObject* new_child(const char* name, Il2CppObject* parent_transform) {
    if (!api.game_object_class || !alive(parent_transform)) return nullptr;
    Il2CppObject* go = il2::il2cpp_object_new(api.game_object_class);
    void* name_args[] = {il2::il2cpp_string_new(name)};
    il2::invoke(api.go_ctor, go, name_args);
    set_int(api.go_set_layer, go, 5);  // NGUI's
    bool keep_world = false;
    void* parent_args[] = {parent_transform, &keep_world};
    il2::invoke(api.set_parent, go_transform(go), parent_args);
    set_vec(api.set_local_position, go_transform(go), {0, 0, 0});
    set_vec(api.set_local_scale, go_transform(go), {1, 1, 1});
    return go;
}

void destroy(Il2CppObject* obj) {
    if (!alive(obj)) return;
    void* args[] = {obj};
    il2::invoke(api.destroy, nullptr, args);
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// Where a screen point (pixels) is in NGUI's world: a widget put there shows at that pixel.
bool place_on_screen(Il2CppObject* transform, Il2CppObject* ui_camera, float sx, float sy) {
    if (!alive(transform) || !alive(ui_camera)) return false;
    Vector3 screen{sx, sy, 0}, world{};
    void* args[] = {&screen};
    if (!il2::invoke_value(api.screen_to_world, ui_camera, world, args)) return false;
    set_vec(api.set_position, transform, world);
    Vector3 local{};
    if (il2::invoke_value(api.get_local_position, transform, local)) set_vec(api.set_local_position, transform, {local.x, local.y, 0});
    return true;
}

int screen_size(const MethodInfo* getter) {
    int v = 0;
    il2::invoke_value(getter, nullptr, v);
    return v;
}

// --- state ---------------------------------------------------------------------------------------------------------

bool g_ready = false;
bool g_map_ready = false, g_radar_ready = false, g_chart_ready = false;  // TLD's map parts found
std::string g_showing = "not in a co-op game";  // for the log: ours shown, or why not
bool g_names = true, g_radar = true, g_chart = true;
std::string g_scene;
bool g_in_game = false;

uintptr_t g_hud = 0;   // Panel_HUD
uintptr_t g_root = 0;  // our GameObject under its NonEssentialHud, with our UIPanel
Color g_text_color{0.86f, 0.84f, 0.78f, 1};  // TLD's hover text colour (read from its HUD)

// One friend in our scene: the dot over their head (the name with it when close or looked at), or the arrow at the edge.
struct Tag {
    uintptr_t go = 0, dot = 0, name = 0, distance = 0, arrow = 0;
    std::string shown_name, shown_distance;
    bool name_wanted = false;  // close, or we look at them: the name fades in
    float name_fade = 0;       // 0 hidden .. 1 shown
    std::string state;         // for the log: "dot over their head", "name over their head", "arrow at the edge"...
};
std::map<uint8_t, Tag> g_tags;
std::chrono::steady_clock::time_point g_last_frame{};

uintptr_t g_elsewhere = 0;  // the friends in other scenes, under the radar
std::string g_elsewhere_text;
std::map<std::string, std::string> g_place_names;  // scene -> TLD's name for it

// TLD's widgets to copy (from its own HUD).
Il2CppObject* hud_part(ptrdiff_t field) { return field >= 0 ? get<Il2CppObject*>(live(g_hud), field) : nullptr; }
Il2CppObject* hud_label() { return hud_part(api.hud_label); }
Il2CppObject* hud_label_back() { return hud_part(api.hud_label_back); }
Il2CppObject* hud_crosshair() { return hud_part(api.hud_crosshair); }

void drop_tag(Tag& t) {
    destroy(live(t.go));
    for (uintptr_t* h : {&t.go, &t.dot, &t.name, &t.distance, &t.arrow}) drop(*h);
}

void forget_radar();

void forget_widgets() {
    forget_radar();
    for (auto& [id, t] : g_tags) drop_tag(t);
    g_tags.clear();
    drop(g_elsewhere);
    g_elsewhere_text.clear();
    destroy(live(g_root));
    drop(g_root);
}

Il2CppObject* hud_panel() {
    if (Il2CppObject* p = live(g_hud)) return p;
    drop(g_hud);
    if (!api.load_panel || !api.hud_type) return nullptr;
    void* args[] = {api.hud_type};
    Il2CppObject* p = il2::invoke(api.load_panel, nullptr, args);
    if (!alive(p)) return nullptr;
    g_hud = hold(p);
    if (api.hud_text_color >= 0) {
        const Color c = get<Color>(p, api.hud_text_color);
        if (c.a > 0) g_text_color = c;
    }
    return p;
}

// Our root under TLD's NonEssentialHud: TLD hides and fades it with the rest of its HUD.
Il2CppObject* root() {
    if (Il2CppObject* r = live(g_root)) return r;
    if (g_root) forget_widgets();
    Il2CppObject* hud = hud_panel();
    Il2CppObject* nonessential = get<Il2CppObject*>(hud, api.hud_nonessential);
    if (!alive(nonessential)) return nullptr;
    Il2CppObject* go = new_child("TLDCoop_FindingEachOther", go_transform(nonessential));
    if (!go) return nullptr;
    void* type_args[] = {api.panel_type};
    Il2CppObject* panel = il2::invoke(api.add_component, go, type_args);
    set_int(api.panel_set_depth, panel, kPanelDepth);
    g_root = hold(go);
    logx::info("hud: made our widgets' root under TLD's HUD (names %s, radar %s, charting %s)", g_names ? "on" : "off",
               g_radar ? "on" : "off", g_chart ? "on" : "off");
    return go;
}

// A label in TLD's hover text look under `parent`.
Il2CppObject* make_label(Il2CppObject* parent_transform, const std::string& name, int font, int pivot) {
    Il2CppObject* label = clone_under(hud_label(), parent_transform, name);
    if (!label) return nullptr;
    set_int(api.label_set_overflow, label, kOverflowResizeFreely);
    set_int(api.label_set_font_size, label, font);
    set_int(api.widget_set_pivot, label, pivot);
    set_vec(api.set_local_position, transform_of(label), {0, 0, 0});
    set_color(label, g_text_color);
    return label;
}

// A copy of TLD's crosshair would follow its UIAnchor back to the screen's centre: the copy goes without it.
void strip_anchor(Il2CppObject* widget) {
    Il2CppObject* go = go_of(widget);
    if (!go || !api.get_component || !api.anchor_type) return;
    void* args[] = {api.anchor_type};
    destroy(il2::invoke(api.get_component, go, args));
}

// A `size` square sprite by its name in TLD's atlases: a copy of one of TLD's own HUD sprites whose atlas has it.
Il2CppObject* make_sprite(Il2CppObject* parent_transform, const std::string& name, const char* sprite, int size) {
    for (Il2CppObject* source : {hud_crosshair(), hud_label_back()}) {
        Il2CppObject* copy = clone_under(source, parent_transform, name);
        if (!copy) continue;
        strip_anchor(copy);
        void* args[] = {il2::il2cpp_string_new(sprite)};
        il2::invoke(api.sprite_set_name, copy, args);
        bool valid = false;
        if (il2::invoke_value(api.sprite_valid, copy, valid) && valid) {
            set_int(api.widget_set_pivot, copy, kPivotCenter);
            set_int(api.widget_set_width, copy, size);
            set_int(api.widget_set_height, copy, size);
            return copy;
        }
        destroy(go_of(copy));
    }
    logx::warn("hud: no atlas of TLD's has the sprite '%s'", sprite);
    return nullptr;
}

bool make_tag(Tag& t, uint8_t player) {
    Il2CppObject* r = root();
    if (!r) return false;
    const std::string base = "Tag" + std::to_string(player);
    Il2CppObject* go = new_child(base.c_str(), go_transform(r));
    if (!go) return false;
    t.go = hold(go);
    Il2CppObject* tf = go_transform(go);
    t.dot = hold(make_sprite(tf, "Dot", "crosshair4", kDotSize));
    t.arrow = hold(make_sprite(tf, "Arrow", "arrow_nav3", kArrowSize));
    t.name = hold(make_label(tf, "Name", kNameFont, kPivotLeft));
    t.distance = hold(make_label(tf, "Distance", kDistanceFont, kPivotLeft));
    t.name_fade = 0;
    return live(t.name) != nullptr;
}

// The name and its distance on one line, centred at (x, y) in the tag's own units.
void lay_out_name(Tag& t, float x, float y, float alpha) {
    Il2CppObject* name = live(t.name);
    Il2CppObject* distance = live(t.distance);
    const Vector2 n = printed(name), d = printed(distance);
    const float left = x - (n.x + kNameGap + d.x) * 0.5f;
    set_vec(api.set_local_position, transform_of(name), {left, y, 0});
    set_vec(api.set_local_position, transform_of(distance), {left + n.x + kNameGap, y - 1, 0});
    Color c = g_text_color;
    c.a = alpha * kNameAlpha;
    set_color(name, c);
    c.a = alpha * kDistanceAlpha;
    set_color(distance, c);
}

void show_tag(uint8_t player, const avatar::Seen& who, const game::PlayerState& me, Il2CppObject* game_camera,
              Il2CppObject* ui_camera, float width, float height, float dt) {
    Tag& t = g_tags[player];
    if (!live(t.go) && !make_tag(t, player)) return;
    Il2CppObject* go = live(t.go);
    const float dx = who.x - me.position.x, dy = who.y - me.position.y, dz = who.z - me.position.z;
    const float metres = std::sqrt(dx * dx + dy * dy + dz * dz);

    Vector3 head{who.x, who.y + kHeadAbove, who.z}, middle{who.x, who.y + kBodyMiddle, who.z}, screen{}, body{};
    void* head_args[] = {&head};
    void* middle_args[] = {&middle};
    if (!il2::invoke_value(api.world_to_screen, game_camera, screen, head_args) ||
        !il2::invoke_value(api.world_to_screen, game_camera, body, middle_args)) {
        set_active(go, false);
        return;
    }
    const hud::Tag at = hud::place_tag({screen.x, screen.y, screen.z}, width, height, height * kEdgeMargin);
    set_active(go, true);
    place_on_screen(go_transform(go), ui_camera, at.x, at.y);

    const std::string name = upper(who.name.empty() ? "Player " + std::to_string(player) : who.name);
    const std::string distance = hud::distance_text(metres);
    if (name != t.shown_name) set_text(live(t.name), t.shown_name = name);
    if (distance != t.shown_distance) set_text(live(t.distance), t.shown_distance = distance);

    t.name_wanted = hud::name_wanted(metres, {body.x, body.y, body.z}, width, height);
    t.name_fade = hud::fade(t.name_fade, t.name_wanted, dt, kFadeSeconds);
    Il2CppObject* dot = live(t.dot);
    Il2CppObject* arrow = live(t.arrow);
    set_active(go_of(dot), at.on_screen);
    set_active(go_of(arrow), !at.on_screen);
    set_active(go_of(live(t.name)), t.name_fade > 0);
    set_active(go_of(live(t.distance)), t.name_fade > 0);
    Color c = g_text_color;
    if (at.on_screen) {
        const float near_far = hud::tag_alpha(metres);  // out of the way up close, fainter far off
        c.a = kDotAlpha * near_far;
        set_color(dot, c);
        if (t.name_fade > 0) lay_out_name(t, 0, kTextAbove, t.name_fade * near_far);
        t.state = t.name_wanted ? "name over their head" : "dot over their head";
        return;
    }
    // At the edge: the arrow their way (arrow_nav3 points right), a close friend's name beside it towards the centre.
    c.a = kArrowAlpha;
    set_color(arrow, c);
    set_vec(api.set_local_euler, transform_of(arrow), {0, 0, at.angle_deg});
    if (t.name_fade > 0) {
        const float a = at.angle_deg * hud::kPi / 180.0f;
        const float half = (printed(live(t.name)).x + kNameGap + printed(live(t.distance)).x) * 0.5f;
        lay_out_name(t, -std::cos(a) * (kArrowToText + half), -std::sin(a) * (kArrowToText + 6), t.name_fade);
    }
    t.state = t.name_wanted ? "arrow and name at the edge" : "arrow at the edge";
}

void hide_tag(Tag& t) {
    set_active(live(t.go), false);
    t.name_fade = 0;
    t.state = "hidden";
}

// TLD's name for a scene ("Camp Office": InterfaceManager.GetNameForScene), else the scene's own name spaced out.
std::string place_name(const std::string& scene) {
    if (auto it = g_place_names.find(scene); it != g_place_names.end()) return it->second;
    std::string out;
    if (api.scene_name) {
        void* args[] = {il2::il2cpp_string_new(scene.c_str())};
        if (Il2CppObject* s = il2::invoke(api.scene_name, nullptr, args))
            out = il2::to_utf8(reinterpret_cast<Il2CppString*>(s));
    }
    if (out.empty()) {
        for (size_t i = 0; i < scene.size(); ++i) {
            if (i > 0 && scene[i] >= 'A' && scene[i] <= 'Z' && scene[i - 1] >= 'a' && scene[i - 1] <= 'z') out += ' ';
            out += scene[i];
        }
    }
    return g_place_names[scene] = out;
}

// Friends in other scenes, one line each, in the top right corner (under the radar when it's on).
void show_elsewhere(const std::vector<avatar::Seen>& all, Il2CppObject* ui_camera, float right_x, float top_y) {
    std::string text;
    for (const avatar::Seen& who : all) {
        if (!who.playing || who.here || who.dead || who.scene == g_scene) continue;
        text += (text.empty() ? "" : "\n") + upper(who.name) + " - " + upper(place_name(who.scene));
    }
    Il2CppObject* label = live(g_elsewhere);
    if (text.empty()) {
        set_active(go_of(label), false);
        return;
    }
    if (!label) {
        Il2CppObject* r = root();
        label = make_label(go_transform(r), "Elsewhere", kElsewhereFont, kPivotTopRight);
        if (!label) return;
        g_elsewhere = hold(label);
    }
    set_active(go_of(label), true);
    if (text != g_elsewhere_text) {
        set_text(label, text);
        logx::info("hud: friends elsewhere: %s", text.c_str());
        g_elsewhere_text = text;
    }
    place_on_screen(transform_of(label), ui_camera, right_x, top_y);
}

// --- TLD's map ---------------------------------------------------------------------------------------------------

constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;  // List<T>: its array, its count

int list_size(Il2CppObject* list) { return list ? get<int32_t>(list, kListSize) : 0; }
Il2CppObject* list_at(Il2CppObject* list, int i) { return il2::array_object_at(get<Il2CppObject*>(list, kListItems), i); }

// Dictionary<string, T>[key], or null without that key (its methods looked up once per dictionary class).
Il2CppObject* dict_get(Il2CppObject* dict, Il2CppString* key) {
    if (!dict || !key) return nullptr;
    static std::map<Il2CppClass*, std::pair<const MethodInfo*, const MethodInfo*>> methods;
    Il2CppClass* klass = il2::il2cpp_object_get_class(dict);
    auto it = methods.find(klass);
    if (it == methods.end())
        it = methods.emplace(klass, std::make_pair(il2::find_method(klass, "ContainsKey", 1), il2::find_method(klass, "get_Item", 1)))
                 .first;
    const auto [contains, item] = it->second;
    bool has = false;
    void* args[] = {key};
    if (!contains || !item || !il2::invoke_value(contains, dict, has, args) || !has) return nullptr;
    return il2::invoke(item, dict, args);
}

uintptr_t g_map = 0;  // Panel_Map

Il2CppObject* map_panel() {
    if (!g_map_ready) return nullptr;
    if (Il2CppObject* p = live(g_map)) return p;
    drop(g_map);
    void* args[] = {api.map_type};
    Il2CppObject* p = il2::invoke(api.load_panel, nullptr, args);
    if (!alive(p)) return nullptr;
    g_map = hold(p);
    return p;
}

// Our scene's region on TLD's map (Panel_Map.GetMapNameOfCurrentScene), worked out once a scene.
struct Region {
    bool known = false;    // worked out for `scene`
    std::string scene;
    std::string name;      // "LakeRegion"
    uintptr_t key = 0;     // the name as a managed string, for the map's dictionaries
    bool mappable = false; // TLD maps it (Panel_Map.SceneCanBeMapped): not indoors
} g_region;

const Region& region_now() {
    if (g_region.known && g_region.scene == g_scene) return g_region;
    drop(g_region.key);
    g_region = Region{};
    Il2CppObject* map = map_panel();
    if (!map) return g_region;
    g_region.known = true;
    g_region.scene = g_scene;
    Il2CppObject* name = il2::invoke(api.map_region, map);
    if (name) {
        g_region.name = il2::to_utf8(reinterpret_cast<Il2CppString*>(name));
        g_region.key = hold(name);
        void* args[] = {name};
        bool can = false;
        g_region.mappable = il2::invoke_value(api.map_can_map, map, can, args) && can;
    }
    logx::info("hud: scene %s is %s on TLD's map", g_scene.c_str(),
               g_region.mappable ? ("region " + g_region.name).c_str() : "not");
    return g_region;
}

Il2CppString* region_key() { return reinterpret_cast<Il2CppString*>(target(g_region.key)); }

// A world point on TLD's map of our region (MapElements' units, y up: Panel_Map.WorldPositionToMapPosition).
bool to_map(Il2CppObject* map, const game::Vec3& world, hud::V2& out) {
    Il2CppString* key = region_key();
    if (!map || !key) return false;
    Vector3 w{world.x, world.y, world.z}, m{};
    void* args[] = {key, &w};
    if (!il2::invoke_value(api.map_to_map, map, m, args)) return false;
    out = {m.x, m.y};
    return true;
}

// The region's charted spots (List<DetailSurveyPosition>), null before the first.
Il2CppObject* region_spots(Il2CppObject* map) { return dict_get(get<Il2CppObject*>(map, api.map_spots), region_key()); }

// TLD's own fog of war for our region: there once the map has made its region maps (they load in the background).
Il2CppObject* region_fog(Il2CppObject* map) { return dict_get(get<Il2CppObject*>(map, api.map_fogs), region_key()); }

// --- charting as we walk ---------------------------------------------------------------------------------------------

// TLD's charcoal survey without the charcoal (Panel_Map.DoNearbyDetailsCheck, as CharcoalItem.DetailSurveyComplete
// calls it): what's around us goes on our own map, in our own save. Nothing of it is sent: each player's map is theirs.
// Each new spot redraws the radar's copy of the map from every spot (one blit each, as TLD's map does when it opens):
// spots 50 m apart, each charting 80 m round, cover the ground without piling up.
constexpr float kChartStepM = 50;  // a new survey spot once we're this far from every one charted before
constexpr float kChartRadiusM = 80;  // how far around it is charted (TLD's charcoal survey: m_DetailSurveyRadiusMeters)

struct Chart {
    bool before = false;  // a spot charted (or found charted) in this scene
    hud::V3 last{};       // where we last looked
    int added = 0;        // spots we added this session
    std::string note;     // for the log
} g_chart_state;

// A charted spot within `within` map units of `at`.
bool charted_near(Il2CppObject* spots, hud::V2 at, float within) {
    for (int i = 0, n = list_size(spots); i < n; ++i) {
        Il2CppObject* s = list_at(spots, i);
        if (!s) continue;
        const float x = static_cast<float>(get<int32_t>(s, api.spot_x)) / 3.0f;
        const float y = static_cast<float>(get<int32_t>(s, api.spot_y)) / 3.0f;
        if (std::hypot(x - at.x, y - at.y) < within) return true;
    }
    return false;
}

// Map units in a metre around `at` (the map's x and z scales averaged).
float map_units_per_metre(Il2CppObject* map, const game::Vec3& at) {
    hud::V2 a, b, c;
    if (!to_map(map, at, a) || !to_map(map, {at.x + 100, at.y, at.z}, b) || !to_map(map, {at.x, at.y, at.z + 100}, c))
        return 0;
    return (std::hypot(b.x - a.x, b.y - a.y) + std::hypot(c.x - a.x, c.y - a.y)) / 200.0f;
}

// Charts where we stand if we've walked kChartStepM from the last spot (or `now`): true if a spot was added.
bool chart(const game::PlayerState& me, bool indoors, bool now) {
    if (!g_chart_ready || indoors || !me.valid) return false;
    const hud::V3 here{me.position.x, me.position.y, me.position.z};
    if (!now && !hud::chart_here(g_chart_state.before, g_chart_state.last, here, kChartStepM)) return false;
    Il2CppObject* map = map_panel();
    const Region& region = region_now();
    if (!map || !region.mappable || !region_fog(map)) return false;  // TLD's map not ready for this region yet
    g_chart_state.before = true;
    g_chart_state.last = here;
    hud::V2 at;
    const float per_m = map_units_per_metre(map, me.position);
    Il2CppObject* spots = region_spots(map);
    if (!now && to_map(map, me.position, at) && per_m > 0 && charted_near(spots, at, kChartStepM * per_m)) return false;
    const int before = list_size(spots);
    float radius = std::min(kChartRadiusM, std::max(1.0f, get<float>(map, api.map_survey_radius)));
    bool force = true, use_override = false, vistas = false;
    Vector3 none{0, 0, 0};
    void* args[] = {&radius, &force, &use_override, &none, &vistas};
    il2::invoke(api.map_survey, map, args);
    const int after = list_size(region_spots(map));
    if (after <= before) return false;
    ++g_chart_state.added;
    char note[160];
    std::snprintf(note, sizeof(note), "charted (%.0f, %.0f) in %s, %.0f m around: %d spots on our map", me.position.x,
                  me.position.z, region.name.c_str(), radius, after);
    g_chart_state.note = note;
    logx::info("hud: %s", note);
    return true;
}

// --- the radar ---------------------------------------------------------------------------------------------------

// A round map in the top right corner, as TLD's map screen looks: our copy of TLD's own hand-drawn region map where
// we've charted, dark where we haven't, turned so where we face is up, its edge smudged dark (TLD's outerGlow_circle);
// us TLD's map arrow at its centre, friends the same arrow in rust red, an arrow on the edge for those beyond it.
// Indoors (no map): the dark disc, friends by bearing and distance.
constexpr int kRadarRadius = 62;            // UI units (the screen is 720 high)
constexpr float kRadarMargin = 20;          // from the screen's top right corner
constexpr float kRadarRangeM = 150;         // outdoors: metres from the centre to the ring
constexpr float kRadarIndoorRangeM = 30;    // indoors
constexpr int kRimSize = kRadarRadius * 2 + 14;
constexpr int kMeSize = 18, kFriendSize = 16, kRimArrowSize = 20;
constexpr int kMaskSize = 64;               // the round mask's texture
constexpr int kClipTextureMask = 1;         // UIDrawCall.Clipping
constexpr int kTextureRGBA32 = 4, kWrapClamp = 1;
constexpr float kMapAlpha = 0.95f;
const Color kUnchartedColor{0.13f, 0.12f, 0.11f, 0.85f};  // TLD's whiteSquare under the map: where it isn't charted
const Color kRimColor{0.06f, 0.055f, 0.05f, 0.95f};       // TLD's outerGlow_circle round it: a charcoal smudge
const Color kFriendColor{0.6f, 0.2f, 0.13f, 1};

struct RadarBlip {
    uintptr_t dot = 0, arrow = 0;
    std::string state;  // for the log
};

struct Radar {
    uintptr_t clip = 0, paper = 0, map = 0;  // under the masking panel
    uintptr_t over = 0, rim = 0, me = 0;     // over it, unmasked
    std::map<uint8_t, RadarBlip> blips;
    uintptr_t mask = 0;  // the round mask (Texture2D)
    std::string state;   // for the log: "map of LakeRegion", "indoors"...
    bool whole = false, no_mask = false;  // dev: the whole region map in it; no round mask
    Vector3 at{};                         // dev: our spot on the copy's texture (uv)
} g_radar_w;

// Our copy of TLD's region map, which draws what we've charted into a texture of its own (TLD's own lets go of its
// texture when the map closes). Made under an inactive holder (so it wakes with its owner set) beside TLD's HUD,
// out of our root (which hides with the HUD: hiding it would throw its texture away), in a panel that shows nothing.
struct FogCopy {
    std::string region;
    uintptr_t holder = 0, go = 0, fog = 0;
    float width = 650, height = 650;  // the original's rect under MapElements: map point -> texture
    Vector3 at{}, scale{1, 1, 1};
    float scene_radius = 0;           // RegionMap.m_RadiusOfScene's length (ProcessList's)
    Il2CppObject* drawn_into = nullptr;  // the texture ProcessList last drew, and from how many spots
    int drawn_spots = -1;
    int age = 0;  // frames since made
} g_fog;

void drop_fog() {
    destroy(live(g_fog.holder));
    for (uintptr_t* h : {&g_fog.holder, &g_fog.go, &g_fog.fog}) drop(*h);
    g_fog = FogCopy{};
}

void forget_radar() {
    for (auto& [id, b] : g_radar_w.blips)
        for (uintptr_t* h : {&b.dot, &b.arrow}) drop(*h);
    g_radar_w.blips.clear();
    for (uintptr_t* h : {&g_radar_w.clip, &g_radar_w.paper, &g_radar_w.map, &g_radar_w.over, &g_radar_w.rim, &g_radar_w.me})
        drop(*h);
}

float scene_radius(Il2CppObject* map, const std::string& region) {
    Il2CppObject* list = get<Il2CppObject*>(map, api.map_objects);
    for (int i = 0, n = list_size(list); i < n; ++i) {
        Il2CppObject* r = list_at(list, i);
        Il2CppObject* name = r ? get<Il2CppObject*>(r, api.region_name) : nullptr;
        if (!name) continue;
        const std::string s = il2::to_utf8(reinterpret_cast<Il2CppString*>(name));
        if (_stricmp(s.c_str(), region.c_str()) != 0) continue;
        const Vector3 v = get<Vector3>(r, api.region_radius);
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
    return 0;
}

bool make_fog(Il2CppObject* map, const std::string& region) {
    drop_fog();
    Il2CppObject* original = region_fog(map);
    Il2CppObject* hud = hud_panel();
    Il2CppObject* original_go = go_of(original);
    if (!original_go || !hud) return false;
    Il2CppObject* holder = new_child("TLDCoop_RadarMap", transform_of(hud));
    if (!holder) return false;
    set_active(holder, false);
    g_fog.holder = hold(holder);
    void* type_args[] = {api.panel_type};
    Il2CppObject* panel = il2::invoke(api.add_component, holder, type_args);
    float none = 0;
    void* alpha_args[] = {&none};
    il2::invoke(api.panel_set_alpha, panel, alpha_args);  // it draws into its texture; we show that texture ourselves

    void* copy_args[] = {original_go, go_transform(holder)};
    Il2CppObject* copy = il2::invoke(api.instantiate, nullptr, copy_args);
    void* fog_args[] = {api.fog_type};
    Il2CppObject* fog = alive(copy) ? il2::invoke(api.get_component, copy, fog_args) : nullptr;
    if (!alive(fog)) {
        drop_fog();
        return false;
    }
    g_fog.go = hold(copy);
    g_fog.fog = hold(fog);
    void* owner_args[] = {map};
    il2::invoke(api.fog_set_owner, fog, owner_args);
    void* tex_args[] = {api.texture_type};
    Il2CppObject* texture = il2::invoke(api.get_component, original_go, tex_args);
    int w = 0, h = 0;
    if (il2::invoke_value(api.widget_width, texture, w) && w > 0) g_fog.width = static_cast<float>(w);
    if (il2::invoke_value(api.widget_height, texture, h) && h > 0) g_fog.height = static_cast<float>(h);
    il2::invoke_value(api.get_local_position, go_transform(original_go), g_fog.at);
    il2::invoke_value(api.get_local_scale, go_transform(original_go), g_fog.scale);
    g_fog.scene_radius = scene_radius(map, region);
    g_fog.region = region;
    set_active(copy, true);
    set_active(holder, true);  // wakes: loads the region's map, makes its texture
    logx::info("hud: radar: copied TLD's %s map (%.0fx%.0f at %.0f,%.0f, scene radius %.0f m)", region.c_str(), g_fog.width,
               g_fog.height, g_fog.at.x, g_fog.at.y, g_fog.scene_radius);
    return true;
}

// Our copy of the region map, drawn up to date (again when we chart, TLD's charcoal does, or its texture is new).
// Its texture, or null while there's nothing to show.
Il2CppObject* fog_texture(Il2CppObject* map, const Region& region) {
    if (!g_radar_ready || !region.mappable) return nullptr;
    if (g_fog.region != region.name || !live(g_fog.fog)) {
        if (!region_fog(map) || !make_fog(map, region.name)) return nullptr;
    }
    Il2CppObject* fog = live(g_fog.fog);
    if (++g_fog.age < 2) return nullptr;  // wakes first (Start finds the camera it draws with)
    Il2CppObject* texture = get<Il2CppObject*>(fog, api.fog_texture);
    Il2CppObject* spots = region_spots(map);
    if (!alive(texture) || !spots) return nullptr;
    const int count = list_size(spots);
    if (texture != g_fog.drawn_into || count != g_fog.drawn_spots) {
        Il2CppObject* vistas = dict_get(get<Il2CppObject*>(map, api.map_vistas), region_key());
        float scene = g_fog.scene_radius, map_radius = 900;  // as Panel_Map.LoadMapElementsForScene passes them
        void* args[] = {spots, vistas, &scene, &map_radius};
        il2::invoke(api.fog_process, fog, args);
        g_fog.drawn_into = texture;
        g_fog.drawn_spots = count;
    }
    return texture;
}

// The round mask the radar's panel clips with: white, opaque inside the circle, a soft pixel at its edge.
Il2CppObject* round_mask() {
    if (Il2CppObject* m = live(g_radar_w.mask)) return m;
    drop(g_radar_w.mask);
    if (!api.texture2d_class) return nullptr;
    Il2CppObject* tex = il2::il2cpp_object_new(api.texture2d_class);
    int size = kMaskSize, format = kTextureRGBA32;
    bool mips = false;
    void* ctor_args[] = {&size, &size, &format, &mips};
    il2::invoke(api.tex_ctor, tex, ctor_args);
    if (!alive(tex)) return nullptr;
    set_int(api.tex_wrap, tex, kWrapClamp);
    const float c = (kMaskSize - 1) * 0.5f, r = kMaskSize * 0.5f - 1.0f;
    for (int y = 0; y < kMaskSize; ++y)
        for (int x = 0; x < kMaskSize; ++x) {
            Color px{1, 1, 1, std::clamp(r - std::hypot(x - c, y - c) + 0.5f, 0.0f, 1.0f)};
            int ix = x, iy = y;
            void* args[] = {&ix, &iy, &px};
            il2::invoke(api.tex_set_pixel, tex, args);
        }
    il2::invoke(api.tex_apply, tex);
    g_radar_w.mask = hold(tex);
    return tex;
}

bool make_radar() {
    Il2CppObject* r = root();
    if (!r) return false;
    destroy(live(g_radar_w.clip));
    destroy(live(g_radar_w.over));
    forget_radar();
    Il2CppObject* clip = new_child("Radar", go_transform(r));
    Il2CppObject* over = new_child("RadarOver", go_transform(r));
    if (!clip || !over) return false;
    g_radar_w.clip = hold(clip);
    g_radar_w.over = hold(over);
    void* type_args[] = {api.panel_type};
    Il2CppObject* panel = il2::invoke(api.add_component, clip, type_args);
    set_int(api.panel_set_depth, panel, kPanelDepth);  // under our root's (the ring and icons over it)
    if (Il2CppObject* mask = g_radar_ready && !g_radar_w.no_mask ? round_mask() : nullptr) {
        set_int(api.panel_set_clipping, panel, kClipTextureMask);
        void* mask_args[] = {mask};
        il2::invoke(api.panel_set_clip_texture, panel, mask_args);
        struct {
            float x, y, z, w;
        } region{0, 0, kRadarRadius * 2.0f, kRadarRadius * 2.0f};
        void* region_args[] = {&region};
        il2::invoke(api.panel_set_clip_region, panel, region_args);
    }
    Il2CppObject* clip_tf = go_transform(clip);
    Il2CppObject* paper = make_sprite(clip_tf, "Uncharted", "whiteSquare", kRadarRadius * 2);
    set_int(api.widget_set_depth, paper, 0);
    set_color(paper, kUnchartedColor);
    g_radar_w.paper = hold(paper);
    if (g_radar_ready) {
        Il2CppObject* map_go = new_child("Map", clip_tf);
        void* tex_args[] = {api.texture_type};
        Il2CppObject* map = map_go ? il2::invoke(api.add_component, map_go, tex_args) : nullptr;
        set_int(api.widget_set_width, map, kRadarRadius * 2);
        set_int(api.widget_set_height, map, kRadarRadius * 2);
        set_int(api.widget_set_depth, map, 1);
        set_color(map, {1, 1, 1, kMapAlpha});
        set_active(map_go, false);
        g_radar_w.map = hold(map);
    }
    Il2CppObject* over_tf = go_transform(over);
    Il2CppObject* rim = make_sprite(over_tf, "Rim", "outerGlow_circle", kRimSize);
    set_color(rim, kRimColor);
    g_radar_w.rim = hold(rim);
    Il2CppObject* me = make_sprite(over_tf, "Me", "ico_map_directional", kMeSize);
    set_int(api.widget_set_depth, me, 60);
    set_color(me, {1, 1, 1, 1});
    g_radar_w.me = hold(me);
    logx::info("hud: made the radar (%s)", g_radar_ready ? "TLD's map in a round mask" : "paper only");
    return true;
}

void hide_radar() {
    set_active(live(g_radar_w.clip), false);
    set_active(live(g_radar_w.over), false);
    g_radar_w.state = "off";
}

// A friend's arrow (or the arrow on the ring) on the radar.
RadarBlip& blip_for(uint8_t player) {
    RadarBlip& b = g_radar_w.blips[player];
    Il2CppObject* over_tf = go_transform(live(g_radar_w.over));
    if (!live(b.dot)) {
        drop(b.dot);
        Il2CppObject* dot = make_sprite(over_tf, "Friend" + std::to_string(player), "ico_map_directional", kFriendSize);
        set_int(api.widget_set_depth, dot, 50);
        set_color(dot, kFriendColor);
        b.dot = hold(dot);
    }
    if (!live(b.arrow)) {
        drop(b.arrow);
        Il2CppObject* arrow = make_sprite(over_tf, "FriendAway" + std::to_string(player), "arrow_nav3", kRimArrowSize);
        set_int(api.widget_set_depth, arrow, 50);
        set_color(arrow, kFriendColor);
        b.arrow = hold(arrow);
    }
    return b;
}

// A direction (radar plane) turned as the radar turns the map: its angle on the radar (0 right, 90 up).
float on_radar_deg(hud::V2 dir, float turn_deg) {
    return (std::atan2(dir.y, dir.x) * 180.0f / hud::kPi) + turn_deg;
}

// The radar in the top right corner. Returns the screen y (pixels) just under its ring.
float show_radar(const std::vector<avatar::Seen>& all, const game::PlayerState& me, bool indoors,
                 Il2CppObject* ui_camera, float width, float height, float px_per_unit) {
    const float r_px = kRadarRadius * px_per_unit, margin_px = kRadarMargin * px_per_unit;
    const float below = height - margin_px - r_px - (kRimSize * 0.5f + 6) * px_per_unit;
    if (!me.valid || (!live(g_radar_w.clip) && !make_radar())) return below;
    Il2CppObject* clip = live(g_radar_w.clip);
    Il2CppObject* over = live(g_radar_w.over);
    set_active(clip, true);
    set_active(over, true);
    const float cx = width - margin_px - r_px, cy = height - margin_px - r_px;
    place_on_screen(go_transform(clip), ui_camera, cx, cy);
    place_on_screen(go_transform(over), ui_camera, cx, cy);

    // The radar's plane: TLD's map of our region outdoors; the world's x and z indoors.
    Il2CppObject* map = map_panel();
    const Region& region = region_now();
    const bool on_map = map && region.mappable && !indoors;
    Il2CppObject* texture = on_map ? fog_texture(map, region) : nullptr;
    auto plane = [&](const game::Vec3& w, hud::V2& out) {
        if (on_map) return to_map(map, w, out);
        out = {w.x, w.z};
        return true;
    };
    const float per_m = on_map ? map_units_per_metre(map, me.position) : 1.0f;
    const float range = (on_map ? kRadarRangeM : kRadarIndoorRangeM) * (per_m > 0 ? per_m : 1.0f);
    hud::V2 here, ahead;
    const hud::V2 f = hud::yaw_forward(me.yaw);
    if (!plane(me.position, here) || !plane({me.position.x + f.x * 10, me.position.y, me.position.z + f.y * 10}, ahead))
        return below;
    const hud::V2 forward{ahead.x - here.x, ahead.y - here.y};
    const hud::RadarView view = hud::radar_view(forward, range, static_cast<float>(kRadarRadius));

    Il2CppObject* map_widget = live(g_radar_w.map);
    set_active(go_of(map_widget), texture != nullptr);
    if (texture) {
        void* tex_args[] = {texture};
        il2::invoke(api.texture_set_main, map_widget, tex_args);
        const float w = g_fog.width * g_fog.scale.x, h = g_fog.height * g_fog.scale.y;
        const float u = 0.5f + (here.x - g_fog.at.x) / w, v = 0.5f + (here.y - g_fog.at.y) / h;
        g_radar_w.at = {u, v, 0};
        struct {
            float x, y, w, h;
        } uv{u - range / w, v - range / h, 2 * range / w, 2 * range / h};
        if (g_radar_w.whole) uv = {0, 0, 1, 1};
        void* uv_args[] = {&uv};
        il2::invoke(api.texture_set_uv, map_widget, uv_args);
        set_vec(api.set_local_euler, transform_of(map_widget), {0, 0, view.rotation_deg});
    }
    g_radar_w.state = texture ? "map of " + region.name + " (" + std::to_string(g_fog.drawn_spots) + " charted spots)"
                              : (on_map ? "paper (nothing charted here yet)" : "indoors (bearing and distance)");

    for (auto& [id, b] : g_radar_w.blips) {
        set_active(go_of(live(b.dot)), false);
        set_active(go_of(live(b.arrow)), false);
        b.state.clear();
    }
    for (const avatar::Seen& who : all) {
        if (!who.here || who.dead) continue;
        hud::V2 there, their_ahead;
        const hud::V2 tf = hud::yaw_forward(who.yaw);
        if (!plane({who.x, who.y, who.z}, there) ||
            !plane({who.x + tf.x * 10, who.y, who.z + tf.y * 10}, their_ahead))
            continue;
        const hud::Blip at =
            hud::radar_blip({there.x - here.x, there.y - here.y}, forward, range, static_cast<float>(kRadarRadius));
        RadarBlip& b = blip_for(who.player);
        const float metres = at.distance / (per_m > 0 ? per_m : 1.0f);
        if (at.inside) {
            Il2CppObject* dot = live(b.dot);
            set_active(go_of(dot), true);
            set_vec(api.set_local_position, transform_of(dot), {at.x, at.y, 0});
            const float heading = on_radar_deg({their_ahead.x - there.x, their_ahead.y - there.y}, view.rotation_deg);
            set_vec(api.set_local_euler, transform_of(dot), {0, 0, heading - 90.0f});  // the arrow points up
            b.state = "on the radar (" + hud::distance_text(metres) + ")";
        } else {
            Il2CppObject* arrow = live(b.arrow);
            set_active(go_of(arrow), true);
            const float a = at.angle_deg * hud::kPi / 180.0f, rim = kRadarRadius + kRimArrowSize * 0.35f;
            set_vec(api.set_local_position, transform_of(arrow), {std::cos(a) * rim, std::sin(a) * rim, 0});
            set_vec(api.set_local_euler, transform_of(arrow), {0, 0, at.angle_deg});  // arrow_nav3 points right
            b.state = "on the ring (" + hud::distance_text(metres) + ")";
        }
    }
    return below;
}

// --- friends on TLD's map screen -----------------------------------------------------------------------------------

// While TLD's map is open on a region where a friend is (our own outdoors, or theirs), their mark on it: TLD's map
// arrow (a copy of its PlayerIcon) in rust red, their name beside it. Ours, under MapElements (it pans and zooms with
// the map); never in TLD's map data, so never saved.
constexpr int kMarkFont = 14;

struct MapMark {
    uintptr_t icon = 0, label = 0;
    std::string shown_name;
    std::string state;  // for the log
};
std::map<uint8_t, MapMark> g_marks;

bool map_open(Il2CppObject* map) {
    bool on = false;
    return map && api.panel_enabled && il2::invoke_value(il2::virtual_method(map, api.panel_enabled), map, on) && on;
}

// The region TLD's map shows (m_UnlockedRegionNames[m_RegionSelectedIndex]).
std::string shown_region(Il2CppObject* map) {
    Il2CppObject* names = get<Il2CppObject*>(map, api.map_regions);
    const int i = api.map_region_index >= 0 ? get<int32_t>(map, api.map_region_index) : -1;
    if (!names || i < 0 || i >= list_size(names)) return {};
    Il2CppObject* name = list_at(names, i);
    return name ? il2::to_utf8(reinterpret_cast<Il2CppString*>(name)) : std::string();
}

void hide_marks() {
    for (auto& [id, m] : g_marks) {
        set_active(go_of(live(m.icon)), false);
        set_active(go_of(live(m.label)), false);
        m.state = "hidden";
    }
}

void show_map_marks(const std::vector<avatar::Seen>& all) {
    Il2CppObject* map = g_map_ready ? live(g_map) : nullptr;  // loaded already (it is, once the map has opened)
    if (!map_open(map)) {
        hide_marks();
        return;
    }
    Il2CppObject* elements = get<Il2CppObject*>(map, api.map_elements);
    Il2CppObject* player_icon = get<Il2CppObject*>(map, api.map_player_icon);
    void* sprite_args[] = {api.sprite_type};
    Il2CppObject* icon_sprite = alive(player_icon) ? il2::invoke(api.get_component, go_of(player_icon), sprite_args) : nullptr;
    const std::string shown = shown_region(map);
    const Region& region = region_now();
    // MapElements zooms by its scale: the names keep their size against it (the arrows zoom as TLD's own icons do).
    Vector3 zoom{1, 1, 1};
    il2::invoke_value(api.get_local_scale, elements, zoom);
    const Vector3 unzoom{zoom.x > 0.01f ? 1 / zoom.x : 1, zoom.y > 0.01f ? 1 / zoom.y : 1, 1};
    for (auto& [id, m] : g_marks) m.state = "not on this map";
    for (const avatar::Seen& who : all) {
        MapMark& m = g_marks[who.player];
        // Their position is in the shown region's world: they're outdoors with us there, or in its own scene.
        const bool there = who.playing && !who.dead && !shown.empty() &&
                           ((who.here && region.mappable && region.name == shown) || who.scene == shown);
        if (!there || !alive(elements)) {
            set_active(go_of(live(m.icon)), false);
            set_active(go_of(live(m.label)), false);
            m.state = "not on this map";
            continue;
        }
        if (!live(m.icon)) {
            drop(m.icon);
            Il2CppObject* icon = clone_under(icon_sprite, elements, "TLDCoop_Friend" + std::to_string(who.player));
            set_color(icon, kFriendColor);
            m.icon = hold(icon);
        }
        if (!live(m.label)) {
            drop(m.label);
            m.label = hold(make_label(elements, "TLDCoop_FriendName" + std::to_string(who.player), kMarkFont, kPivotLeft));
            m.shown_name.clear();
        }
        Il2CppObject* icon = live(m.icon);
        Il2CppObject* label = live(m.label);
        Il2CppObject* key = reinterpret_cast<Il2CppObject*>(il2::il2cpp_string_new(shown.c_str()));
        const hud::V2 f = hud::yaw_forward(who.yaw);
        Vector3 at{who.x, who.y, who.z}, ahead{who.x + f.x * 10, who.y, who.z + f.y * 10}, mp{}, mp_ahead{};
        void* at_args[] = {key, &at};
        void* ahead_args[] = {key, &ahead};
        if (!il2::invoke_value(api.map_to_map, map, mp, at_args) || !il2::invoke_value(api.map_to_map, map, mp_ahead, ahead_args))
            continue;
        set_active(go_of(icon), true);
        set_active(go_of(label), true);
        set_vec(api.set_local_position, transform_of(icon), {mp.x, mp.y, 0});
        const float heading = std::atan2(mp_ahead.y - mp.y, mp_ahead.x - mp.x) * 180.0f / hud::kPi;
        set_vec(api.set_local_euler, transform_of(icon), {0, 0, heading - 90.0f});  // the arrow points up
        set_vec(api.set_local_position, transform_of(label), {mp.x + 12, mp.y, 0});
        set_vec(api.set_local_scale, transform_of(label), unzoom);
        const std::string name = upper(who.name);
        if (name != m.shown_name) set_text(label, m.shown_name = name);
        char s[96];
        std::snprintf(s, sizeof(s), "on the %s map at (%.0f, %.0f)", shown.c_str(), mp.x, mp.y);
        m.state = s;
    }
}

// TLD shows its HUD (not hidden in its settings or for a screenshot, no menu over the game).
bool hud_showing(Il2CppObject* hud) {
    Il2CppObject* nonessential = get<Il2CppObject*>(hud, api.hud_nonessential);
    if (!active(nonessential)) return false;
    const int mode = get<int>(hud, api.hud_mode);
    if (mode == kHudOff || mode == kHudScreenShot) return false;
    bool overlay = false;
    if (api.overlay && il2::invoke_value(api.overlay, nullptr, overlay) && overlay) return false;
    return true;
}

bool coop_on() { return netrt::role().has_value(); }

}  // namespace

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* camera = il2::find_class(kCore, "UnityEngine", "Camera");
    Il2CppClass* screen = il2::find_class(kCore, "UnityEngine", "Screen");
    api.game_object_class = game_object;
    api.go_ctor = il2::find_method_params(game_object, ".ctor", {"System.String"});
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.go_active = il2::find_method(game_object, "get_activeInHierarchy", 0);
    api.go_set_layer = il2::find_method(game_object, "set_layer", 1);
    api.add_component = il2::find_method_params(game_object, "AddComponent", {"System.Type"});
    api.get_component = il2::find_method_params(game_object, "GetComponent", {"System.Type"});
    api.instantiate = il2::find_method_params(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
    api.destroy = il2::find_method_params(object, "Destroy", {"UnityEngine.Object"});
    api.set_name = il2::find_method(object, "set_name", 1);
    api.set_parent = il2::find_method_params(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.get_local_position = il2::find_method(transform, "get_localPosition", 0);
    api.set_local_position = il2::find_method(transform, "set_localPosition", 1);
    api.set_local_euler = il2::find_method(transform, "set_localEulerAngles", 1);
    api.set_local_scale = il2::find_method(transform, "set_localScale", 1);
    api.world_to_screen = il2::find_method_params(camera, "WorldToScreenPoint", {"UnityEngine.Vector3"});
    api.screen_to_world = il2::find_method_params(camera, "ScreenToWorldPoint", {"UnityEngine.Vector3"});
    api.screen_width = il2::find_method(screen, "get_width", 0);
    api.screen_height = il2::find_method(screen, "get_height", 0);

    auto keep_type = [](Il2CppClass* klass) {
        Il2CppObject* type = klass ? il2::type_object(klass) : nullptr;
        if (type) il2::il2cpp_gchandle_new(type, false);
        return type;
    };
    Il2CppClass* panel = il2::find_class(kGame, "", "UIPanel");
    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    Il2CppClass* widget = il2::find_class(kGame, "", "UIWidget");
    Il2CppClass* sprite = il2::find_class(kGame, "", "UISprite");
    api.panel_type = keep_type(panel);
    api.anchor_type = keep_type(il2::find_class(kGame, "", "UIAnchor"));
    api.panel_set_depth = il2::find_method(panel, "set_depth", 1);
    api.ui_camera = il2::find_method(il2::find_class(kGame, "", "UICamera"), "get_mainCamera", 0);
    api.label_set_text = il2::find_method(label, "set_text", 1);
    api.label_set_font_size = il2::find_method(label, "set_fontSize", 1);
    api.label_set_overflow = il2::find_method(label, "set_overflowMethod", 1);
    api.label_printed = il2::find_method(label, "get_printedSize", 0);
    api.widget_set_pivot = il2::find_method(widget, "set_pivot", 1);
    api.widget_set_width = il2::find_method(widget, "set_width", 1);
    api.widget_set_height = il2::find_method(widget, "set_height", 1);
    api.widget_set_depth = il2::find_method(widget, "set_depth", 1);
    api.widget_get_color = il2::find_method(widget, "get_color", 0);
    api.widget_set_color = il2::find_method(widget, "set_color", 1);
    api.sprite_set_name = il2::find_method(sprite, "set_spriteName", 1);
    api.sprite_valid = il2::find_method(sprite, "get_isValid", 0);
    api.widget_width = il2::find_method(widget, "get_width", 0);
    api.widget_height = il2::find_method(widget, "get_height", 0);
    api.get_local_scale = il2::find_method(transform, "get_localScale", 0);
    api.pixel_adjustment =
        il2::find_method_params(il2::find_class(kGame, "", "UIRoot"), "GetPixelSizeAdjustment", {"UnityEngine.GameObject"});

    Il2CppClass* ui_texture = il2::find_class(kGame, "", "UITexture");
    api.texture_type = keep_type(ui_texture);
    api.panel_set_alpha = il2::find_method(panel, "set_alpha", 1);
    api.panel_set_clipping = il2::find_method(panel, "set_clipping", 1);
    api.panel_set_clip_texture = il2::find_method(panel, "set_clipTexture", 1);
    api.panel_set_clip_region = il2::find_method(panel, "set_baseClipRegion", 1);
    api.texture_set_main = il2::find_method(ui_texture, "set_mainTexture", 1);
    api.texture_set_uv = il2::find_method(ui_texture, "set_uvRect", 1);
    Il2CppClass* texture2d = il2::find_class(kCore, "UnityEngine", "Texture2D");
    api.texture2d_class = texture2d;
    api.tex_ctor = il2::find_method_params(texture2d, ".ctor",
                                           {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"});
    api.tex_set_pixel = il2::find_method_params(texture2d, "SetPixel", {"System.Int32", "System.Int32", "UnityEngine.Color"});
    api.tex_apply = il2::find_method(texture2d, "Apply", 0);
    api.tex_wrap = il2::find_method(il2::find_class(kCore, "UnityEngine", "Texture"), "set_wrapMode", 1);

    Il2CppClass* map = il2::find_class(kGame, "", "Panel_Map");
    Il2CppClass* fog = il2::find_class(kGame, "", "FogOfWar");
    Il2CppClass* region = il2::find_class(kGame, "", "RegionMap");
    Il2CppClass* spot = il2::find_class(kGame, "", "DetailSurveyPosition");
    api.map_type = keep_type(map);
    api.fog_type = keep_type(fog);
    api.map_to_map = il2::find_method_params(map, "WorldPositionToMapPosition", {"System.String", "UnityEngine.Vector3"});
    api.map_survey = il2::find_method(map, "DoNearbyDetailsCheck", 5);
    api.map_region = il2::find_method(map, "GetMapNameOfCurrentScene", 0);
    api.map_can_map = il2::find_method(map, "SceneCanBeMapped", 1);
    api.map_elements = il2::field_offset(map, "m_MapElementsTransform");
    api.map_player_icon = il2::field_offset(map, "m_PlayerIcon");
    api.map_survey_radius = il2::field_offset(map, "m_DetailSurveyRadiusMeters");
    api.map_objects = il2::field_offset(map, "m_MapObjects");
    api.map_spots = il2::field_offset(map, "m_DetailSurveyPositions");
    api.map_fogs = il2::field_offset(map, "m_FogOfWar");
    api.map_vistas = il2::field_offset(map, "m_SurveyedVistaLocations");
    api.map_regions = il2::field_offset(map, "m_UnlockedRegionNames");
    api.map_region_index = il2::field_offset(map, "m_RegionSelectedIndex");
    api.fog_set_owner = il2::find_method(fog, "SetOwner", 1);
    api.fog_process = il2::find_method(fog, "ProcessList", 4);
    api.fog_texture = il2::field_offset(fog, "m_RevealedMapTex");
    api.fog_camera = il2::field_offset(fog, "m_Camera");
    Il2CppClass* texture_class = il2::find_class(kCore, "UnityEngine", "Texture");
    api.texture_width = il2::find_method(texture_class, "get_width", 0);
    api.texture_height = il2::find_method(texture_class, "get_height", 0);
    api.region_name = il2::field_offset(region, "m_RegionName");
    api.region_radius = il2::field_offset(region, "m_RadiusOfScene");
    api.spot_x = il2::field_offset(spot, "x");
    api.spot_y = il2::field_offset(spot, "y");
    api.map_enable = il2::find_method_params(map, "Enable", {"System.Boolean"});
    api.map_centre = il2::find_method_params(map, "CenterOnPoint", {"UnityEngine.Vector3"});
    Il2CppClass* hud_manager = il2::find_class(kGame, "", "HUDManager");
    api.hud_get_mode = il2::find_method(hud_manager, "get_HudDisplayMode", 0);
    api.hud_set_mode = il2::find_method(hud_manager, "SetHUDDisplayMode", 1);
    api.panel_enabled = il2::find_method(il2::find_class(kGame, "", "Panel_Base"), "IsEnabled", 0);
    api.sprite_type = keep_type(sprite);
    g_map_ready = api.map_type && api.map_to_map && api.map_region && api.map_can_map && api.map_fogs >= 0 &&
                  api.map_spots >= 0 && api.map_objects >= 0 && api.region_name >= 0 && api.region_radius >= 0;
    g_radar_ready = g_map_ready && api.fog_type && api.fog_set_owner && api.fog_process && api.fog_texture >= 0 &&
                    api.texture_type && api.texture_set_main && api.texture_set_uv && api.panel_set_clipping &&
                    api.panel_set_clip_texture && api.panel_set_clip_region && api.panel_set_alpha && api.tex_ctor &&
                    api.tex_set_pixel && api.tex_apply && api.widget_width && api.widget_height && api.get_local_scale;
    g_chart_ready = g_map_ready && api.map_survey && api.map_survey_radius >= 0 && api.spot_x >= 0 && api.spot_y >= 0;
    if (!g_map_ready) logx::warn("hud: TLD's map parts weren't found; no radar map, charting or friends on the map");
    else if (!g_radar_ready) logx::warn("hud: some of the radar's parts weren't found; the radar without TLD's map");
    if (g_map_ready && !g_chart_ready) logx::warn("hud: TLD's survey wasn't found; no charting as we walk");

    Il2CppClass* hud_class = il2::find_class(kGame, "", "Panel_HUD");
    Il2CppClass* interface_manager = il2::find_class(kGame, "", "InterfaceManager");
    api.hud_type = keep_type(hud_class);
    api.load_panel = il2::find_method_params(interface_manager, "LoadPanel", {"System.Type"});
    api.overlay = il2::find_method(interface_manager, "IsOverlayActiveCached", 0);
    api.scene_name = il2::find_method_params(interface_manager, "GetNameForScene", {"System.String"});
    api.main_camera = il2::find_method(il2::find_class(kGame, "", "GameManager"), "GetMainCamera", 0);
    api.hud_nonessential = il2::field_offset(hud_class, "m_NonEssentialHud");
    api.hud_label = il2::field_offset(hud_class, "m_Label_ObjectName");
    api.hud_label_back = il2::field_offset(hud_class, "m_HoverTextBG");
    api.hud_crosshair = il2::field_offset(hud_class, "m_Sprite_Crosshair");
    api.hud_text_color = il2::field_offset(hud_class, "m_HoverTextNormalColor");
    api.hud_mode = il2::field_offset(hud_class, "m_HudDisplayMode");

    g_ready = api.go_ctor && api.add_component && api.instantiate && api.world_to_screen && api.screen_to_world &&
              api.panel_type && api.label_set_text && api.load_panel && api.hud_type && api.main_camera &&
              api.ui_camera && api.hud_nonessential >= 0 && api.hud_label >= 0 && api.hud_label_back >= 0;
    if (!g_ready) logx::warn("hud: some of TLD's HUD parts weren't found; no names, radar or charting");
    return g_ready;
}

void set_switches(bool names, bool radar, bool chart) {
    g_names = names;
    g_radar = radar;
    g_chart = chart;
    logx::info("hud: names %s, radar %s, charting %s", names ? "on" : "off", radar ? "on" : "off", chart ? "on" : "off");
}

void on_frame(const std::string& scene, bool in_game) {
    if (scene != g_scene) g_chart_state.before = false;  // a new scene: chart where we come in
    g_scene = scene;
    g_in_game = in_game;
    if (!g_ready) return;
    if (!in_game) {
        // A menu or a load: TLD's HUD may go with the scene; ours is made again in the game. Our copy of the region
        // map lets go of its textures.
        if (g_root && !live(g_root)) forget_widgets();
        if (g_fog.holder) drop_fog();
    }
}

// Indoors (Weather.IsIndoorEnvironment), looked at twice a second.
bool indoors_now() {
    static std::chrono::steady_clock::time_point next{};
    static bool indoors = false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        indoors = game::read_world().indoors;
        next = now + std::chrono::milliseconds(500);
    }
    return indoors;
}

void late_update() {
    if (!g_ready) return;
    const bool coop = g_in_game && coop_on();
    const game::PlayerState me = coop ? game::read_player() : game::PlayerState{};
    const std::vector<avatar::Seen> all = coop ? avatar::seen() : std::vector<avatar::Seen>{};
    if (coop && g_chart && me.valid) chart(me, indoors_now(), false);  // whether or not TLD shows its HUD
    if (coop) show_map_marks(all);  // TLD's map is open over the game: its HUD is hidden then
    else hide_marks();

    Il2CppObject* hud = (coop && (g_names || g_radar)) ? hud_panel() : nullptr;
    if (!hud) {
        set_active(live(g_root), false);
        g_showing = coop ? "names and radar off" : "not in a co-op game";
        return;
    }
    Il2CppObject* r = root();
    if (!r) return;
    const bool showing = hud_showing(hud);
    set_active(r, showing);
    g_showing = showing ? "shown" : "hidden with TLD's HUD";
    if (!showing) return;

    Il2CppObject* game_camera = il2::invoke(api.main_camera, nullptr);
    Il2CppObject* ui_camera = il2::invoke(api.ui_camera, nullptr);
    const float width = static_cast<float>(screen_size(api.screen_width));
    const float height = static_cast<float>(screen_size(api.screen_height));
    if (!alive(game_camera) || !alive(ui_camera) || width < 1 || height < 1) return;
    float units_a_pixel = 0;
    void* root_args[] = {r};
    if (!api.pixel_adjustment || !il2::invoke_value(api.pixel_adjustment, nullptr, units_a_pixel, root_args) ||
        units_a_pixel <= 0)
        units_a_pixel = 720.0f / height;
    const float px_per_unit = 1.0f / units_a_pixel;

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - g_last_frame).count(), 0.0f, 0.1f);
    g_last_frame = now;

    float below_radar = height - kRadarMargin * px_per_unit;
    if (g_radar) {
        below_radar = show_radar(all, me, indoors_now(), ui_camera, width, height, px_per_unit);
    } else {
        hide_radar();
        if (g_fog.holder) drop_fog();  // its textures go with it
    }
    for (auto& [id, t] : g_tags) {
        const bool listed = std::any_of(all.begin(), all.end(), [&](const avatar::Seen& s) { return s.player == id; });
        if (!listed) hide_tag(t);
    }
    for (const avatar::Seen& who : all) {
        if (g_names && who.here && !who.dead && me.valid)
            show_tag(who.player, who, me, game_camera, ui_camera, width, height, dt);
        else if (auto it = g_tags.find(who.player); it != g_tags.end())
            hide_tag(it->second);
    }
    show_elsewhere(all, ui_camera, width - kRadarMargin * px_per_unit, below_radar);
}

std::string describe() {
    std::string out = std::string("names ") + (g_names ? "on" : "off") + ", radar " + (g_radar ? "on" : "off") +
                      ", charting " + (g_chart ? "on" : "off") + " (" + g_showing + ")";
    for (const auto& [id, t] : g_tags)
        out += "; player " + std::to_string(id) + " '" + t.shown_name + "' " + t.state +
               (t.state == "hidden" ? "" : " (" + t.shown_distance + ")");
    if (g_radar) {
        out += "; radar: " + (g_radar_w.state.empty() ? std::string("not made") : g_radar_w.state);
        if (Il2CppObject* fog = live(g_fog.fog)) {
            Il2CppObject* texture = get<Il2CppObject*>(fog, api.fog_texture);
            int w = 0, h = 0;
            if (alive(texture)) {
                il2::invoke_value(api.texture_width, texture, w);
                il2::invoke_value(api.texture_height, texture, h);
            }
            const Vector3 at = g_radar_w.at;
            char s[200];
            std::snprintf(s, sizeof(s), " [copy's texture %dx%d, camera %s, we're at uv %.3f,%.3f]", w, h,
                          alive(get<Il2CppObject*>(fog, api.fog_camera)) ? "found" : "none", at.x, at.y);
            out += s;
        }
        for (const auto& [id, b] : g_radar_w.blips)
            if (!b.state.empty()) out += ", player " + std::to_string(id) + " " + b.state;
    }
    if (!g_elsewhere_text.empty()) out += "; elsewhere: " + g_elsewhere_text;
    for (const auto& [id, m] : g_marks)
        if (m.state.rfind("on the ", 0) == 0) out += "; TLD's map: player " + std::to_string(id) + " " + m.state;
    out += "; charting: " + std::to_string(g_chart_state.added) + " spots added this session" +
           (g_chart_state.note.empty() ? "" : ", last " + g_chart_state.note);
    return out;
}

bool dev(const std::string& action, const std::string& arg) {
    const bool on = arg == "on" || arg == "1";
    if (action == "chart" && arg == "now") {
        const game::PlayerState me = game::read_player();
        const bool indoors = game::read_world().indoors;
        Il2CppObject* map = map_panel();
        const Region& region = region_now();
        const bool added = chart(me, indoors, true);
        logx::info("hud: chart now: %s (region %s, %s, TLD's survey radius %.0f m)",
                   added ? g_chart_state.note.c_str() : "nothing added", region.name.c_str(),
                   indoors ? "indoors" : (region.mappable ? "mappable" : "not mappable"),
                   map ? get<float>(map, api.map_survey_radius) : 0.0f);
        return true;
    }
    if (action == "radar" && (arg.rfind("rim ", 0) == 0 || arg.rfind("paper ", 0) == 0)) {
        // Try one of TLD's sprites as the ring (over the map) or the paper (under it): NAME [R G B A], or none.
        std::istringstream in(arg);
        std::string part, sprite;
        Color c{1, 1, 1, 1};
        in >> part >> sprite >> c.r >> c.g >> c.b >> c.a;
        Il2CppObject* widget = live(part == "rim" ? g_radar_w.rim : g_radar_w.paper);
        set_active(go_of(widget), sprite != "none");
        if (sprite != "none" && widget) {
            void* args[] = {il2::il2cpp_string_new(sprite.c_str())};
            il2::invoke(api.sprite_set_name, widget, args);
            set_color(widget, c);
        }
        bool valid = false;
        il2::invoke_value(api.sprite_valid, widget, valid);
        logx::info("hud: radar %s: %s (%s), colour %.2f %.2f %.2f %.2f", part.c_str(), sprite.c_str(),
                   valid ? "in the atlas" : "not in the atlas", c.r, c.g, c.b, c.a);
        return true;
    }
    if (action == "tld" && api.hud_get_mode && api.hud_set_mode) {
        // TLD's HUD hidden as its screenshot key hides it (runtime only, nothing saved), then back as it was.
        static int before = -1;
        int mode = 0;
        il2::invoke_value(api.hud_get_mode, nullptr, mode);
        int to = arg == "hide" ? kHudScreenShot : (before >= 0 ? before : 2);
        if (arg == "hide" && before < 0) before = mode;
        if (arg != "hide") before = -1;
        void* args[] = {&to};
        il2::invoke(api.hud_set_mode, nullptr, args);
        logx::info("hud: TLD's HUD display mode %d -> %d", mode, to);
        return true;
    }
    if (action == "map" && arg == "centre") {
        // TLD's open map moved to where we stand (Panel_Map.CenterOnPoint, a world point), as a test wants to see it.
        Il2CppObject* map = map_panel();
        const game::PlayerState me = game::read_player();
        if (!map_open(map) || !api.map_centre || !me.valid) return false;
        Vector3 at{me.position.x, me.position.y, me.position.z};
        void* args[] = {&at};
        il2::invoke(api.map_centre, map, args);
        logx::info("hud: TLD's map centred on us (%.0f, %.0f)", at.x, at.z);
        return true;
    }
    if (action == "map") {
        // TLD's map screen opened or closed as its own Enable does (the radial's map opens it so).
        Il2CppObject* map = map_panel();
        if (!map || !api.map_enable) return false;
        bool open = arg != "close";
        void* args[] = {&open};
        il2::invoke(il2::virtual_method(map, api.map_enable), map, args);
        logx::info("hud: TLD's map %s (it shows %s)", map_open(map) ? "open" : "closed", shown_region(map).c_str());
        return true;
    }
    if (action == "radar" && (arg == "whole" || arg == "noclip" || arg == "normal")) {
        g_radar_w.whole = arg == "whole";
        g_radar_w.no_mask = arg == "noclip";
        make_radar();
        logx::info("hud: radar %s", arg.c_str());
        return true;
    }
    if (action == "names") g_names = on;
    else if (action == "radar") g_radar = on;
    else if (action == "chart") g_chart = on;
    else return false;
    logx::info("hud: %s", describe().c_str());
    return true;
}

}  // namespace coop_hud
