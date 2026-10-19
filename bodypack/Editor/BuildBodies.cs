// Builds tldcoop_bodies: one walking prefab per story character, for TLDCoop's remote-player avatar.
// Run headless:  Unity.exe -batchmode -nographics -quit -projectPath <project> -executeMethod TLDCoop.BuildBodies.Build
//
// Inputs (project root): bodies.json (characters + idle/walk/run clips) and shader_map.json (each material's
// original TLD shader, from tools/collect_assets.py). Output: Build/tldcoop_bodies.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;
using UnityEngine.Playables;

namespace TLDCoop {

[Serializable] class BodyEntry { public string id; public string gender; public string prefab; }
[Serializable] class BodyList { public string[] clipsM; public string[] clipsF; public string clipDown; public BodyEntry[] bodies; }
[Serializable] class ShaderEntry { public string material; public string shader; }
[Serializable] class ShaderMap { public ShaderEntry[] entries; }

public static class BuildBodies {
    const string OutDir = "Assets/BodyPack";
    const string BundleName = "tldcoop_bodies";
    const string BuildDir = "Build";

    public static void Build() {
        try {
            Run();
        } catch (Exception e) {
            Debug.LogError("[BodyPack] FAILED: " + e);
            EditorApplication.Exit(1);
        }
    }

    static void Run() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        var shaderMap = JsonUtility.FromJson<ShaderMap>(File.ReadAllText("shader_map.json"));
        Directory.CreateDirectory(OutDir);

        // Remove bodies from earlier builds that are no longer listed, so they can't end up in the bundle.
        var listed = new HashSet<string>();
        foreach (var b in list.bodies) listed.Add($"{OutDir}/Body_{b.id}.prefab");
        foreach (var guid in AssetDatabase.FindAssets("Body_ t:Prefab", new[] { OutDir })) {
            string stale = AssetDatabase.GUIDToAssetPath(guid);
            if (!listed.Contains(stale) && AssetDatabase.DeleteAsset(stale)) Debug.Log($"[BodyPack] removed unlisted {stale}");
        }

        int fixedMaterials = AssignFallbackShaders(shaderMap);
        var gaits = new Dictionary<string, Gait> {
            ["M"] = MakeGait("M", list.clipsM, list.clipDown),
            ["F"] = MakeGait("F", list.clipsF, list.clipDown),
        };

        int built = 0;
        foreach (var body in list.bodies) {
            var source = AssetDatabase.LoadAssetAtPath<GameObject>(body.prefab);
            if (source == null) {
                Debug.LogError($"[BodyPack] missing prefab for '{body.id}': {body.prefab}");
                continue;
            }
            var go = (GameObject)UnityEngine.Object.Instantiate(source);
            go.name = "Body_" + body.id;
            foreach (var t in go.GetComponentsInChildren<Transform>(true))
                GameObjectUtility.RemoveMonoBehavioursWithMissingScript(t.gameObject);

            // Legacy Animation, not Mecanim: through an Animator (ripped or freshly built Generic avatar alike)
            // these ripped clips flip the upper body and fling bones; sampled by bone path, which is what the
            // legacy component does, they play exactly as on their reference rig (see DiagnoseAnimatorEffect).
            var animator = go.GetComponent<Animator>();
            if (animator) UnityEngine.Object.DestroyImmediate(animator);
            var gait = gaits[body.gender == "F" ? "F" : "M"];
            var animation = go.AddComponent<Animation>();
            AnimationUtility.SetAnimationClips(animation, new[] { gait.idle, gait.walk, gait.run, gait.down });
            animation.clip = gait.idle;
            animation.playAutomatically = true;
            animation.wrapMode = WrapMode.Loop;
            // Always animate: a body Unity wrongly thinks is off-screen would freeze, then jump.
            animation.cullingType = AnimationCullingType.AlwaysAnimate;
            foreach (var smr in go.GetComponentsInChildren<SkinnedMeshRenderer>(true))
                smr.updateWhenOffscreen = true;  // bounds from the live bones: ripped bounds can't cull us wrongly
            // The mod reads the clips' ground speeds from this marker to match foot speed to movement.
            new GameObject($"TLDCoopGait walk={gait.walkSpeed:F2} run={gait.runSpeed:F2}").transform.SetParent(go.transform, false);

            string path = $"{OutDir}/Body_{body.id}.prefab";
            PrefabUtility.SaveAsPrefabAsset(go, path);
            UnityEngine.Object.DestroyImmediate(go);
            AssetImporter.GetAtPath(path).SetAssetBundleNameAndVariant(BundleName, "");
            built++;
        }
        AssetDatabase.SaveAssets();

        // Refuse to ship bodies that contort: play each through its real controller and check the pose.
        var broken = new List<string>();
        foreach (var body in list.bodies) {
            string verdict = VerifyBody($"{OutDir}/Body_{body.id}.prefab");
            Debug.Log($"[BodyPack] verify {body.id}: {verdict}");
            if (!verdict.StartsWith("OK")) broken.Add(body.id);
        }
        if (broken.Count > 0) throw new Exception("bodies failed the pose check: " + string.Join(", ", broken));

        // AssetRipper keeps each asset's original Addressables bundle name in its .meta; clear them so every
        // dependency (meshes, textures, clips) is packed into our one bundle instead of TLD-named side bundles.
        foreach (var other in AssetDatabase.GetAllAssetBundleNames())
            if (other != BundleName) AssetDatabase.RemoveAssetBundleName(other, true);
        AssetDatabase.RemoveUnusedAssetBundleNames();
        if (Directory.Exists(BuildDir)) Directory.Delete(BuildDir, true);

        Directory.CreateDirectory(BuildDir);
        var manifest = BuildPipeline.BuildAssetBundles(BuildDir,
            BuildAssetBundleOptions.ChunkBasedCompression | BuildAssetBundleOptions.StrictMode,
            BuildTarget.StandaloneWindows64);
        if (manifest == null) throw new Exception("BuildAssetBundles returned null");
        if (manifest.GetAllAssetBundles().Length != 1)
            throw new Exception("expected exactly one bundle, got: " + string.Join(", ", manifest.GetAllAssetBundles()));
        var size = new FileInfo(Path.Combine(BuildDir, BundleName)).Length;
        Debug.Log($"[BodyPack] built {built}/{list.bodies.Length} bodies, {fixedMaterials} materials, " +
                  $"bundle {size / 1048576.0:F1} MB");
    }

    // How each locomotion clip moves the root and hips (run with -executeMethod TLDCoop.BuildBodies.DiagnoseClips).
    public static void DiagnoseClips() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        foreach (var path in list.clipsM) {
            var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(path);
            var bindings = AnimationUtility.GetCurveBindings(clip);
            int pos = 0, rot = 0, other = 0;
            var notes = new List<string>();
            foreach (var b in bindings) {
                if (b.propertyName.StartsWith("m_LocalPosition")) pos++;
                else if (b.propertyName.StartsWith("m_LocalRotation") || b.propertyName.StartsWith("localEulerAngles")) rot++;
                else other++;
                bool interesting = b.path.EndsWith("Origin_jnt") || b.path.EndsWith("Hips_jnt") || b.path == "";
                if (interesting && b.propertyName.EndsWith(".x") || interesting && b.propertyName.EndsWith(".z") ||
                    b.path == "" ) {
                    var curve = AnimationUtility.GetEditorCurve(clip, b);
                    if (curve == null || curve.length == 0) continue;
                    float first = curve.keys[0].value, last = curve.keys[curve.length - 1].value, min = first, max = first;
                    foreach (var k in curve.keys) { min = Mathf.Min(min, k.value); max = Mathf.Max(max, k.value); }
                    notes.Add($"{(b.path == "" ? "<root>" : b.path.Substring(b.path.LastIndexOf('/') + 1))}.{b.propertyName}: {first:F2}->{last:F2} [{min:F2},{max:F2}]");
                }
            }
            Debug.Log($"[DiagClip] {clip.name}: length {clip.length:F2}s loop={clip.isLooping} bindings {bindings.Length} " +
                      $"(pos {pos}, rot {rot}, other {other}) avgSpeed {clip.averageSpeed} | " + string.Join(" | ", notes));
        }
        // Do the character and clip rigs agree? Compare Mackenzie's rest bone offsets with the walk clip's first frame.
        var walk = AssetDatabase.LoadAssetAtPath<AnimationClip>(list.clipsM[1]);
        var mack = AssetDatabase.LoadAssetAtPath<GameObject>(list.bodies[0].prefab);
        int compared = 0; float worst = 0; string worstBone = "";
        foreach (var b in AnimationUtility.GetCurveBindings(walk)) {
            if (b.propertyName != "m_LocalPosition.y") continue;
            var t = mack.transform.Find(b.path);
            if (t == null) continue;
            float clipValue = AnimationUtility.GetEditorCurve(walk, b).keys[0].value;
            float diff = Mathf.Abs(clipValue - t.localPosition.y);
            compared++;
            if (diff > worst) { worst = diff; worstBone = b.path.Substring(b.path.LastIndexOf('/') + 1); }
        }
        Debug.Log($"[DiagClip] rest vs walk frame 0, {compared} bones: worst local-position.y mismatch {worst:F3} m at {worstBone}");
    }

    // Position curves vs the rig's rest offsets: bones whose animated local position strays far from rest
    // (or holds absurd values) stretch or fling the skeleton.
    public static void DiagnosePositions() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        var rigs = new Dictionary<string, GameObject> {
            ["M"] = AssetDatabase.LoadAssetAtPath<GameObject>(list.bodies[0].prefab),   // mackenzie
            ["F"] = AssetDatabase.LoadAssetAtPath<GameObject>(list.bodies[4].prefab),   // molly
        };
        foreach (var (gender, clips) in new[] { ("M", list.clipsM), ("F", list.clipsF) }) {
            foreach (var clipPath in clips) {
                var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(clipPath);
                var findings = new List<(float, string)>();
                foreach (var b in AnimationUtility.GetCurveBindings(clip)) {
                    if (!b.propertyName.StartsWith("m_LocalPosition")) continue;
                    var curve = AnimationUtility.GetEditorCurve(clip, b);
                    var bone = rigs[gender].transform.Find(b.path);
                    char axis = b.propertyName[b.propertyName.Length - 1];
                    float restValue = bone == null ? float.NaN
                                    : axis == 'x' ? bone.localPosition.x : axis == 'y' ? bone.localPosition.y : bone.localPosition.z;
                    float worst = 0, worstKey = 0, maxTangent = 0;
                    foreach (var k in curve.keys) {
                        float d = Mathf.Abs(k.value - restValue);
                        if (float.IsNaN(d) || d > worst) { worst = float.IsNaN(d) ? 999 : d; worstKey = k.value; }
                        maxTangent = Mathf.Max(maxTangent, Mathf.Abs(k.inTangent), Mathf.Abs(k.outTangent));
                    }
                    // Sample between keys too: absurd tangents overshoot there.
                    float sampledMax = 0;
                    for (int f = 0; f <= 240; f++) sampledMax = Mathf.Max(sampledMax, Mathf.Abs(curve.Evaluate(clip.length * f / 240f)));
                    if (worst > 0.1f || sampledMax > 3f)
                        findings.Add((Mathf.Max(worst, sampledMax), $"{b.path.Substring(b.path.LastIndexOf('/') + 1)}.{axis}: rest {restValue:F3}, key {worstKey:F3}, sampled max {sampledMax:F1}, keys {curve.length}, max tangent {maxTangent:F1}"));
                }
                findings.Sort((a, c) => c.Item1.CompareTo(a.Item1));
                Debug.Log($"[DiagPos] {clip.name}: {findings.Count} position curves off rest by >10cm or sampled >3m. " +
                          string.Join(" || ", findings.GetRange(0, Mathf.Min(8, findings.Count)).ConvertAll(f => f.Item2)));
            }
        }
    }

    // Sample the same clip on the clip's own reference rig and on a character, and print where key joints end up.
    public static void DiagnoseRigs() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        var clips = new[] { list.clipsM[0], list.clipsM[1] };
        var rigs = new[] { ("NPC_M reference", "Assets/RigCheck/NPC_M_Walk.prefab"), ("mackenzie", list.bodies[0].prefab) };
        string[] joints = { "Hips_jnt", "Spine1_jnt", "Spine3_jnt", "Head_jnt", "LeftHip_jnt", "LeftKnee_jnt", "LeftFoot_jnt", "LeftUpArm_jnt" };
        foreach (var clipPath in clips) {
            var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(clipPath);
            foreach (var (label, path) in rigs) {
                var go = (GameObject)UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(path));
                foreach (float t in new[] { -1f, 0f, clip.length * 0.5f }) {
                    if (t >= 0) {
                        AnimationMode.StartAnimationMode();
                        clip.SampleAnimation(go, t);
                    }
                    var parts = new List<string>();
                    foreach (var j in joints) {
                        var tr = Find(go.transform, j);
                        parts.Add(tr ? $"{j.Replace("_jnt", "")}({tr.position.x:F2},{tr.position.y:F2},{tr.position.z:F2})" : $"{j}:missing");
                    }
                    Debug.Log($"[Rig] {clip.name} on {label} at {(t < 0 ? "rest" : t.ToString("F2") + "s")}: {string.Join(" ", parts)}");
                    if (t >= 0) AnimationMode.StopAnimationMode();
                }
                UnityEngine.Object.DestroyImmediate(go);
            }
        }
    }

    // Does the Animator component (with its avatar) change how a clip samples? Same clip and time, with and
    // without the Animator, on the reference rig and on Mackenzie.
    public static void DiagnoseAnimatorEffect() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        var original = AssetDatabase.LoadAssetAtPath<AnimationClip>(list.clipsM[0]);
        var rebuilt = AssetDatabase.LoadAssetAtPath<AnimationClip>($"{OutDir}/M/idle.anim");
        foreach (var (rigLabel, rigPath) in new[] { ("reference", "Assets/RigCheck/NPC_M_Walk.prefab"), ("mackenzie", list.bodies[0].prefab) }) {
            foreach (var (clipLabel, clip) in new[] { ("original", original), ("rebuilt", rebuilt) }) {
                foreach (bool keepAnimator in new[] { true, false }) {
                    var go = (GameObject)UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(rigPath));
                    var animator = go.GetComponent<Animator>();
                    string animatorInfo = animator ? $"animator(avatar {(animator.avatar ? animator.avatar.name : "none")})" : "no animator";
                    if (!keepAnimator && animator) { UnityEngine.Object.DestroyImmediate(animator); animatorInfo = "animator removed"; }
                    AnimationMode.StartAnimationMode();
                    clip.SampleAnimation(go, 3.35f);
                    var head = Find(go.transform, "Head_jnt").position;
                    var foot = Find(go.transform, "LeftFoot_jnt").position;
                    AnimationMode.StopAnimationMode();
                    Debug.Log($"[AnimFx] {clipLabel} idle@3.35 on {rigLabel}, {animatorInfo}: head y {head.y:F2}, left foot y {foot.y:F2}");
                    UnityEngine.Object.DestroyImmediate(go);
                }
            }
        }
    }

    // Run the pose check on the bodies as they are now, without rebuilding.
    public static void VerifyExisting() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        foreach (var body in list.bodies)
            Debug.Log($"[Verify] {body.id}: {VerifyBody($"{OutDir}/Body_{body.id}.prefab")}");
    }

    // Play every clip of a built body the way the legacy Animation component does (by bone path) and check each
    // frame that it stays a person: head above hips and feet below hips while upright, on the ground with the hips
    // low in the struggle ("down"), no bone flung away, no NaN, and the pose actually changes (it animates).
    public static string VerifyBody(string prefabPath) {
        var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
        var go = (GameObject)UnityEngine.Object.Instantiate(prefab);
        try {
            var animation = go.GetComponent<Animation>();
            Transform hips = Find(go.transform, "Hips_jnt"), head = Find(go.transform, "Head_jnt");
            Transform lfoot = Find(go.transform, "LeftFoot_jnt"), rfoot = Find(go.transform, "RightFoot_jnt");
            if (!animation || !hips || !head || !lfoot || !rfoot) return "FAIL: missing Animation component or key joints";
            if (go.GetComponent<Animator>()) return "FAIL: still has an Animator";
            var clips = AnimationUtility.GetAnimationClips(go);
            if (clips.Length != 4) return $"FAIL: expected idle/walk/run/down, found {clips.Length} clips";
            var bones = go.GetComponentsInChildren<Transform>(true);
            var rest = new Vector3[bones.Length];
            for (int i = 0; i < bones.Length; i++) rest[i] = bones[i].position;

            // Sample by bone path, as the legacy Animation component plays them.
            float minHeadAboveHips = 9, maxFootAboveHips = -9, maxReach = 0, maxMove = 0;
            float minDownJointHeight = 9, maxDownHipHeight = 0;
            string farthest = "";
            AnimationMode.StartAnimationMode();
            foreach (var clip in clips) {
                for (int f = 0; f <= 60; f++) {
                    AnimationMode.BeginSampling();
                    AnimationMode.SampleAnimationClip(go, clip, clip.length * f / 60f);
                    AnimationMode.EndSampling();
                    if (clip.name != "down") {
                        minHeadAboveHips = Mathf.Min(minHeadAboveHips, head.position.y - hips.position.y);
                        maxFootAboveHips = Mathf.Max(maxFootAboveHips, Mathf.Max(lfoot.position.y, rfoot.position.y) - hips.position.y);
                    } else {
                        // Ground poses deliberately put head/feet near the hips' height.
                        minDownJointHeight = Mathf.Min(minDownJointHeight, head.position.y,
                            hips.position.y, lfoot.position.y, rfoot.position.y);
                        maxDownHipHeight = Mathf.Max(maxDownHipHeight, hips.position.y);
                    }
                    for (int i = 0; i < bones.Length; i++) {
                        var p = bones[i].position;
                        if (float.IsNaN(p.x + p.y + p.z)) {
                            AnimationMode.StopAnimationMode();
                            return $"FAIL: NaN at {bones[i].name} in {clip.name}";
                        }
                        float reach = (p - go.transform.position).magnitude;
                        if (reach > maxReach) { maxReach = reach; farthest = bones[i].name; }
                        maxMove = Mathf.Max(maxMove, (p - rest[i]).magnitude);
                    }
                }
            }
            AnimationMode.StopAnimationMode();
            string stats = $"head-hips min {minHeadAboveHips:F2} m, feet-hips max {maxFootAboveHips:F2} m, " +
                           $"reach {maxReach:F2} m ({farthest}), moved {maxMove:F2} m";
            if (minDownJointHeight < 0 || maxDownHipHeight > 0.55f)
                return $"FAIL (ground pose): min joint y {minDownJointHeight:F3}, max hips y {maxDownHipHeight:F3}";
            if (minHeadAboveHips < 0.3f) return "FAIL (flips/contorts): " + stats;
            if (maxFootAboveHips > 0.1f) return "FAIL (legs over hips): " + stats;
            if (maxReach > 2.5f) return "FAIL (bones flung away): " + stats;
            if (maxMove < 0.02f) return "FAIL (doesn't animate): " + stats;
            return "OK: " + stats;
        } finally {
            UnityEngine.Object.DestroyImmediate(go);
        }
    }

    static Transform Find(Transform root, string name) {
        foreach (var t in root.GetComponentsInChildren<Transform>(true)) if (t.name == name) return t;
        return null;
    }

    // Quaternion health of a clip: sign flips between consecutive keys (component-wise interpolation then passes
    // near zero and the bone spins), and how far frame 0 is from a character's rest pose.
    public static void DiagnoseRotations() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        var walk = AssetDatabase.LoadAssetAtPath<AnimationClip>(list.clipsM[1]);
        var mack = AssetDatabase.LoadAssetAtPath<GameObject>(list.bodies[0].prefab);
        var flips = new List<(string bone, int flips, int keys, float restAngle)>();
        foreach (var b in AnimationUtility.GetCurveBindings(walk)) {
            if (b.propertyName != "m_LocalRotation.x") continue;
            var cx = AnimationUtility.GetEditorCurve(walk, b);
            var cy = AnimationUtility.GetEditorCurve(walk, EditorCurveBinding.FloatCurve(b.path, b.type, "m_LocalRotation.y"));
            var cz = AnimationUtility.GetEditorCurve(walk, EditorCurveBinding.FloatCurve(b.path, b.type, "m_LocalRotation.z"));
            var cw = AnimationUtility.GetEditorCurve(walk, EditorCurveBinding.FloatCurve(b.path, b.type, "m_LocalRotation.w"));
            if (cx == null || cy == null || cz == null || cw == null) continue;
            int n = Mathf.Min(cx.length, Mathf.Min(cy.length, Mathf.Min(cz.length, cw.length))), count = 0;
            for (int i = 1; i < n; i++) {
                float dot = cx.keys[i].value * cx.keys[i - 1].value + cy.keys[i].value * cy.keys[i - 1].value +
                            cz.keys[i].value * cz.keys[i - 1].value + cw.keys[i].value * cw.keys[i - 1].value;
                if (dot < 0) count++;
            }
            var t = mack.transform.Find(b.path);
            float angle = -1;
            if (t != null) {
                var q0 = new Quaternion(cx.keys[0].value, cy.keys[0].value, cz.keys[0].value, cw.keys[0].value).normalized;
                angle = Quaternion.Angle(q0, t.localRotation);
            }
            flips.Add((b.path.Substring(b.path.LastIndexOf('/') + 1), count, n, angle));
        }
        int bonesWithFlips = 0, totalFlips = 0;
        foreach (var f in flips) if (f.flips > 0) { bonesWithFlips++; totalFlips += f.flips; }
        flips.Sort((a, c) => c.flips.CompareTo(a.flips));
        var top = flips.GetRange(0, Mathf.Min(12, flips.Count)).ConvertAll(f => $"{f.bone}:{f.flips}/{f.keys}");
        Debug.Log($"[DiagRot] {walk.name}: {flips.Count} rotated bones, {bonesWithFlips} with sign flips ({totalFlips} total). Worst: {string.Join(" ", top)}");
        flips.Sort((a, c) => c.restAngle.CompareTo(a.restAngle));
        var off = flips.GetRange(0, Mathf.Min(12, flips.Count)).ConvertAll(f => $"{f.bone}:{f.restAngle:F0}deg");
        Debug.Log($"[DiagRot] frame 0 vs Mackenzie rest, largest differences: {string.Join(" ", off)}");
    }

    // Sample each bone's rotation at 120 Hz and look for spikes between frames (bad tangents make curves
    // overshoot between keys) and non-unit quaternions (components interpolated far off the unit sphere).
    public static void DiagnoseCurves() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        foreach (var clipPath in new[] { list.clipsM[1], list.clipsM[2], list.clipsM[0] }) {
            var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(clipPath);
            var report = new List<(float spike, string text)>();
            foreach (var b in AnimationUtility.GetCurveBindings(clip)) {
                if (b.propertyName != "m_LocalRotation.x") continue;
                var c = new AnimationCurve[4];
                string[] comp = { "x", "y", "z", "w" };
                for (int i = 0; i < 4; i++)
                    c[i] = AnimationUtility.GetEditorCurve(clip, EditorCurveBinding.FloatCurve(b.path, b.type, "m_LocalRotation." + comp[i]));
                if (Array.Exists(c, x => x == null)) continue;
                float maxStep = 0, minNorm = 9, maxNorm = 0, maxTangent = 0;
                Quaternion prev = Quaternion.identity;
                for (int f = 0; f <= Mathf.CeilToInt(clip.length * 120); f++) {
                    float t = Mathf.Min(f / 120f, clip.length);
                    var raw = new Vector4(c[0].Evaluate(t), c[1].Evaluate(t), c[2].Evaluate(t), c[3].Evaluate(t));
                    float norm = raw.magnitude;
                    minNorm = Mathf.Min(minNorm, norm); maxNorm = Mathf.Max(maxNorm, norm);
                    var q = new Quaternion(raw.x, raw.y, raw.z, raw.w).normalized;
                    if (f > 0) maxStep = Mathf.Max(maxStep, Quaternion.Angle(prev, q));
                    prev = q;
                }
                foreach (var curve in c)
                    foreach (var k in curve.keys)
                        maxTangent = Mathf.Max(maxTangent, Mathf.Abs(k.inTangent), Mathf.Abs(k.outTangent));
                string bone = b.path.Substring(b.path.LastIndexOf('/') + 1);
                report.Add((maxStep, $"{bone}: keys {c[0].length}, max step {maxStep:F1} deg/frame, |q| {minNorm:F2}..{maxNorm:F2}, max tangent {maxTangent:F1}"));
            }
            report.Sort((a, d) => d.spike.CompareTo(a.spike));
            int spiky = report.FindAll(r => r.spike > 10).Count;
            Debug.Log($"[DiagCurve] {clip.name}: {report.Count} bones, {spiky} with >10 deg jumps per 1/120 s. Worst: " +
                      string.Join(" || ", report.GetRange(0, Mathf.Min(6, report.Count)).ConvertAll(r => r.text)));
        }
    }

    // What Unity actually sees in the ripped assets (run with -executeMethod TLDCoop.BuildBodies.Diagnose).
    public static void Diagnose() {
        var list = JsonUtility.FromJson<BodyList>(File.ReadAllText("bodies.json"));
        foreach (var body in list.bodies) {
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(body.prefab);
            if (prefab == null) { Debug.Log($"[Diag] {body.id}: prefab missing"); continue; }
            var smrs = prefab.GetComponentsInChildren<SkinnedMeshRenderer>(true);
            var animator = prefab.GetComponent<Animator>();
            var avatar = animator ? animator.avatar : null;
            string meshes = "";
            foreach (var smr in smrs) {
                var mesh = smr.sharedMesh;
                meshes += $"[{smr.name}: mesh={(mesh ? mesh.name + " v" + mesh.vertexCount + " sub" + mesh.subMeshCount : "NULL")}" +
                          $" bones={smr.bones.Length} mats={smr.sharedMaterials.Length}";
                foreach (var m in smr.sharedMaterials) {
                    var tex = m ? m.GetTexture("_MainTex") : null;
                    meshes += $" ({(m ? m.name : "NULL")}:{(tex ? tex.width + "x" + tex.height : "noTex")})";
                }
                meshes += "] ";
            }
            Debug.Log($"[Diag] {body.id}: avatar={(avatar ? avatar.name : "NULL")} valid={(avatar ? avatar.isValid : false)} " +
                      $"human={(avatar ? avatar.isHuman : false)} smr={smrs.Length} {meshes}");
            if (body.id == "mackenzie" && avatar == null) {
                var raw = AssetDatabase.LoadAllAssetsAtPath(body.prefab.Replace(".prefab", "Avatar.asset").Replace("Mackenzie_Ep5/Mackenzie_Ep5Avatar", "Mackenzie_Ep5/Mackenzie_Ep5Avatar"));
                foreach (var o in raw) Debug.Log($"[Diag]   avatar file object: {o?.GetType().Name} {o?.name}");
            }
            break;  // the first character is enough to see what's wrong
        }
    }

    // The ripped materials point at TLD shaders we don't ship; give each a fallback of the same kind. The mod
    // swaps TLD's real shader back in at runtime when the game has it loaded.
    static int AssignFallbackShaders(ShaderMap map) {
        var opaque = Shader.Find("TLDCoop/Body");
        var twoSided = Shader.Find("TLDCoop/Body2Sided");
        var transparent = Shader.Find("TLDCoop/BodyTransparent");
        if (!opaque || !twoSided || !transparent) throw new Exception("TLDCoop fallback shaders didn't compile");
        int n = 0;
        foreach (var e in map.entries) {
            var material = AssetDatabase.LoadAssetAtPath<Material>(e.material);
            if (material == null) continue;
            var shader = e.shader.Contains("Transparent") || e.shader.Contains("Glass") ? transparent
                       : e.shader.Contains("2Sided") ? twoSided
                       : opaque;
            material.shader = shader;
            EditorUtility.SetDirty(material);
            n++;
        }
        AssetDatabase.SaveAssets();
        return n;
    }

    class Gait {
        public AnimationClip idle, walk, run;
        public AnimationClip down;  // on the ground (sitting), while the player fights an animal
        public float walkSpeed, runSpeed;  // m/s the original clips' root motion covers
    }

    // Legacy, in-place idle/walk/run clips named "idle", "walk", "run" (the Animation component's state names),
    // plus the ground speeds the mod uses to scale playback.
    static Gait MakeGait(string gender, string[] clips, string downClip) {
        var idle = LoadLooping(clips[0]);
        var walk = LoadLooping(clips[1]);
        var run = LoadLooping(clips[2]);
        var gait = new Gait {
            walkSpeed = GroundSpeed(walk, 1.4f),
            idle = InPlace(idle, gender, "idle"),
            walk = InPlace(walk, gender, "walk"),
            run = InPlace(run, gender, "run"),
            // Original, in-place struggle authored on the existing NPC rig.
            down = LoadStruggle(gender),
        };
        gait.runSpeed = Mathf.Max(GroundSpeed(run, 3.8f), gait.walkSpeed + 0.5f);
        Debug.Log($"[BodyPack] gait {gender}: walk {gait.walkSpeed:F2} m/s, run {gait.runSpeed:F2} m/s");
        return gait;
    }

    static AnimationClip LoadStruggle(string gender) {
        string path = $"Assets/AnimalStruggle/{gender}/down.anim";
        var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(path);
        if (clip == null || !clip.legacy)
            throw new Exception("Import AnimalStruggle_Clips.unitypackage first; missing legacy clip: " + path);
        clip.wrapMode = WrapMode.Loop;
        return clip;
    }

    static AnimationClip LoadLooping(string path) {
        var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(path);
        if (clip == null) throw new Exception("missing clip " + path);
        var settings = AnimationUtility.GetAnimationClipSettings(clip);
        if (!settings.loopTime) {
            settings.loopTime = true;
            AnimationUtility.SetAnimationClipSettings(clip, settings);
            EditorUtility.SetDirty(clip);
        }
        return clip;
    }

    // A clean copy of an NPC clip for the body pack:
    //  - in place: the clips walk Origin_jnt forward (1.67 m per walk loop) and snap back each loop, and the mod
    //    moves the body itself, so that travel and the root-motion curves are dropped;
    //  - rebuilt from the editor curves into a brand-new clip, with non-finite or absurd tangents (AssetRipper
    //    writes Infinity for stepped keys) replaced, so the Animator's compiled clip matches what the curves say;
    //  - quaternion continuity enforced.
    static AnimationClip InPlace(AnimationClip source, string gender, string state, bool keepOriginHeight = false) {
        string dir = $"{OutDir}/{gender}";
        Directory.CreateDirectory(dir);
        string path = $"{dir}/{state}.anim";  // the asset (and so the clip and state) name is the file name
        AssetDatabase.DeleteAsset(path);
        var clip = new AnimationClip { name = state, frameRate = source.frameRate, legacy = true };
        int removed = 0, kept = 0, fixedTangents = 0;
        foreach (var b in AnimationUtility.GetCurveBindings(source)) {
            bool originTravel = b.path.EndsWith("Origin_jnt") && b.propertyName.StartsWith("m_LocalPosition") &&
                                !(keepOriginHeight && b.propertyName == "m_LocalPosition.y");
            bool rootMotion = b.path == "" && (b.propertyName.StartsWith("MotionT") || b.propertyName.StartsWith("MotionQ") ||
                                               b.propertyName.StartsWith("RootT") || b.propertyName.StartsWith("RootQ"));
            if (originTravel || rootMotion) { removed++; continue; }
            var keys = AnimationUtility.GetEditorCurve(source, b).keys;
            for (int i = 0; i < keys.Length; i++) {
                if (!IsSane(keys[i].inTangent)) { keys[i].inTangent = 0; fixedTangents++; }
                if (!IsSane(keys[i].outTangent)) { keys[i].outTangent = 0; fixedTangents++; }
                keys[i].weightedMode = WeightedMode.None;
            }
            AnimationUtility.SetEditorCurve(clip, b, new AnimationCurve(keys));
            kept++;
        }
        clip.EnsureQuaternionContinuity();
        clip.wrapMode = WrapMode.Loop;
        AssetDatabase.CreateAsset(clip, path);
        Debug.Log($"[BodyPack] {gender}/{state} (from {source.name}): {kept} curves, removed {removed} root-travel curves, " +
                  $"fixed {fixedTangents} tangents");
        return clip;
    }

    static bool IsSane(float tangent) => !float.IsNaN(tangent) && !float.IsInfinity(tangent) && Mathf.Abs(tangent) < 1000f;

    static float GroundSpeed(AnimationClip clip, float fallback) {
        var v = clip.averageSpeed;
        float s = new Vector2(v.x, v.z).magnitude;
        return s > 0.2f ? s : fallback;
    }
}

}  // namespace TLDCoop
