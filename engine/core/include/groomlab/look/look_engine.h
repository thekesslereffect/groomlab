// SPDX-License-Identifier: Apache-2.0
#pragma once

// The look engine: look.slang's kernels run over a look run (look_run.h) from the strands entering
// it, on a backend's few primitives (buffers by number, a dispatch, a copy, timestamps): one run
// algorithm (look_engine.cpp). The CPU's backend here (makeCpuLookEngine: the kernels compiled to
// C++); the GPU's, DirectX 11 and OpenGL, in engine/gpu (groomlab/gpu/look_engine.h), where a viewport
// previews a drag with it, the results straight into the host's vertex buffers (LookTargets).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/look/look_run.h"

namespace groomlab::gpu {

enum class GpuApi { D3D11, OpenGL, Cpu };
const char* apiName(GpuApi api);

// Milliseconds on the GPU (timestamp queries) of the last run, per kind of node (summed over
// nodes of one kind; clumpMs counts making the centers; clearanceMs is Interpolate's walk keeping
// its strands clear of the scalp, after its blend), the lines' frames, and copyMs: the
// results copied into targets the kernels could not write (0 when they wrote them in place).
struct GpuTimes {
    bool valid = false;
    double interpolateMs = 0, clearanceMs = 0, lengthMs = 0, clumpMs = 0, noiseMs = 0, curlMs = 0, widthMs = 0, partMs = 0;
    double gravityMs = 0, flyawayMs = 0, collideMs = 0, windMs = 0, blendMs = 0, selfCollideMs = 0, staticMs = 0;
    double bendMs = 0, directionMs = 0, transformMs = 0, displaceMs = 0, twistMs = 0;
    double smoothMs = 0, restoreLengthMs = 0, blendNeighborsMs = 0, tangleMs = 0, flowMs = 0;  // the shapers reading their neighbours or a reference
    double jiggleMs = 0, meshCutMs = 0;
    double layoutMs = 0;  // Cull, Split, Mirror: the strands of the run up their input copied into their layout
    double linesMs = 0, copyMs = 0, textureMs = 0, totalMs = 0;
};

// Where a run's results go: the host's vertex buffers, float3 per point, at least the base's
// point count long. DirectX 11: ID3D11Buffer*; written in place when it was made for that
// (createD3D11DisplayBuffer: unordered access and raw views), else copied into on the GPU.
// OpenGL: buffer names (GLuint, cast through std::uintptr_t), always written in place (bound as
// shader storage). Null: the results stay in the engine (checks read them back).
//
// `pointTexture`: the standalone app's point texture instead of (or beside) the buffers: an
// RGBA32F texture `textureWidth` texels wide holding two texels a point, (x y z strand) and
// (tangent xyz, width), as its lookdev renderer builds it on the CPU (buildHairBuffers). After
// the run a kernel writes the final points there in that layout (the widths Width's where it
// ran, else the base's). DirectX 11: ID3D11Texture2D* made with unordered access (QRhi's
// UsedWithLoadStore); OpenGL: a texture name (GLuint, cast), bound as an image. Vulkan and Metal
// hosts to come: the same kernel through the engine of their API.
//
// `uvs`: the strand lines' uvs, float2 per point (u, width: display.h StrandLines::writeUvs), whose
// widths the lines' coverage is drawn from. Where a Width runs, its widths go into each point's
// second float (the u the host wrote stays). Written in place only (DirectX 11: a buffer made for
// it, createD3D11DisplayBuffer; OpenGL: any); one the kernels cannot write is left alone.
struct LookTargets {
    void* positions = nullptr;
    void* normals = nullptr;
    void* tangents = nullptr;
    void* bitangents = nullptr;
    void* pointTexture = nullptr;
    std::uint32_t textureWidth = 0;
    void* uvs = nullptr;
    // (The CPU engine: arrays in memory, written in place; `widths` a float a point, where a Width
    // runs; `baseAsOutput`: the host holds the base's points itself (an operator's own run), so a
    // run moving none leaves its output the base, no copy made.)
    void* widths = nullptr;
    bool baseAsOutput = false;
};

// Everything a run takes beyond the base: the chain's params, its mapped params' values and what
// Interpolate and Clump read from the CPU's operators (look_chain.h), each stage at its index.
struct LookRun {
    LookParams params;
    LookFields fields;
    std::vector<StageInputs> inputs;
};

// The compute engine on a device: the base strands' buffers, the kernels, the outputs. Runs on the
// caller's thread (Maya: the main thread, inside the draw override's update, with the renderer's
// context) and leaves the compute state it touches as it found it.
struct StageConstants;  // (look_engine_internal.h)

class LookEngine {
public:
    virtual ~LookEngine();
    LookEngine(const LookEngine&) = delete;
    LookEngine& operator=(const LookEngine&) = delete;

    virtual GpuApi api() const = 0;
    // The device it runs on: ID3D11Device*, or the OpenGL context (HGLRC) it was made in.
    virtual void* device() const = 0;
    // Whether its objects can be used from the caller now (OpenGL: from the context current, the
    // one it was made in or one sharing with it; false once that is gone). Its other calls throw
    // std::runtime_error when not.
    virtual bool reachable() const { return true; }
    // Whether the kernels came from the program binary cache (OpenGL: look_engine_gl.cpp), not
    // compiled by the driver here (DirectX 11's are bytecode: false).
    virtual bool kernelsFromCache() const { return false; }
    // Whether the kernels a run of `run` dispatches are ready: OpenGL compiles them the first time in
    // the background (prepareGLLookKernels), and until they are a host draws the CPU's rather than
    // wait; `why` then says which are compiling. Throws std::runtime_error when one did not compile.
    // (run itself waits for them.)
    bool kernelsReady(const LookRun& run, std::string* why = nullptr);

    // Uploads the strands entering the chain and their root normals (as the lit lines take them),
    // unless they are the ones held. Strands a preview holds hidden (preview_core.h: the core it
    // does not show) are run like any and left out of the results: the targets hold the shown
    // strands' points alone, as the viewport's buffers do (no point texture then: run throws). `changed`: how `base` differs from the strands held (the
    // graph's changesSince for the base node): where only some strands' points moved (a brush
    // stroke's incremental evaluation), only those go up. Where the graph cannot say but the
    // layout, IDs and roots are the ones held, the points are compared strand by strand to find
    // them. Returns true when it uploaded.
    bool setBase(const StrandsPtr& base, const ScalpMesh* scalp, const StrandChanges* changed = nullptr);
    // The strands `which` of `base` alone (a sharing engine's: an operator's incremental evaluation
    // of a stage whose strands each read only themselves): a run's strand k is base's which[k], its
    // points where base holds them, its result written there in the targets (the rest left as they
    // are). The next setBase leaves the view.
    void setBaseView(const StrandsPtr& base, const ScalpMesh* scalp, std::shared_ptr<const std::vector<std::uint32_t>> which);
    const StrandsPtr& base() const { return base_; }
    // Whether the last upload sent only the changed strands, and how many strands it sent.
    bool lastUploadPartial() const { return uploadPartial_; }
    std::size_t lastUploadStrands() const { return uploadStrands_; }

    // Runs `run`'s chain on the base strands into `targets` (null ones: the engine's own buffers);
    // `frames`: also the lit lines' normals, tangents and bitangents. What it does not hold of the
    // run goes up first (each node's per-strand numbers, drawn from the strands' IDs and its seed;
    // the mapped values, by their serial; Interpolate's binding and guides, Clump's centers, by
    // pointer). Timestamps bracket each node. Throws std::runtime_error (values read on other
    // strands than the base, more nodes than it runs).
    void run(const LookRun& run, bool frames, const LookTargets& targets = {});
    // Whether the last run wrote its targets in place (no copy).
    bool wroteInPlace() const { return inPlace_; }
    // Whether the last run wrote widths into the uvs target (a Width ran, the target writable).
    bool wroteUvs() const { return uvsWritten_; }
    // Lets go of the targets the last run used (views on them, bindings): before the host frees them.
    virtual void dropTargets() = 0;
    // A sharing engine (the CPU's) between an operator's runs: holds its base only while someone
    // else does (a weak hold), so the strands it ran on are freed with the graph's results (a render
    // keeping no intermediates); the same strands set again find what was made of them still there.
    void letGoOfBase();
    // The last run's GPU times; `wait` blocks until the GPU has them (else invalid when not ready).
    GpuTimes times(bool wait);
    // Waits until the GPU has done everything submitted.
    virtual void finish() = 0;
    // Off: Noise, Width, Blend, the frames and the point texture run a thread per strand (tests compare both);
    // on, a thread per point, the strands' point counts the same or not.
    void setPointThreads(bool on) { pointThreads_ = on; }
    // The most groups a dispatch's row takes (a device's limit, 65535, by default): a dispatch of
    // more runs as rows of them. Tests lower it to run rows on few strands.
    void setDispatchLimit(std::size_t groups) { dispatchLimit_ = std::max<std::size_t>(1, std::min<std::size_t>(groups, kMaxGroups)); }
    static constexpr std::size_t kMaxGroups = 65535;

    // The last base upload: CPU milliseconds (packing and the copy to the driver) and megabytes.
    double uploadMs() const { return uploadMs_; }
    double uploadMegabytes() const { return uploadMegabytes_; }
    std::size_t uploads() const { return uploads_; }
    // What the last run sent besides the base (per-strand numbers, values, binding, guides,
    // centers): megabytes and CPU milliseconds.
    double runUploadMegabytes() const { return runUploadMegabytes_; }
    double runUploadMs() const { return runUploadMs_; }
    // ... of which (cumulative ms): the per-strand numbers, then the mapped values, then the stages' inputs.
    const double* prepareParts() const { return prepareParts_; }
    double fieldMegabytes() const { return fieldMegabytes_; }
    std::uint64_t fieldsSerial() const { return fieldSerial_; }
    // Interpolate keeping clearance, the last run: whether it walked only the strands bound to the
    // guides it changed (the rest kept from the run before), and how many guides of how many changed.
    struct WalkStats {
        bool partial = false;
        std::size_t changedGuides = 0, guides = 0;
    };
    const WalkStats& lastWalk() const { return lastWalk_; }
    // Video memory held by the engine's own buffers (bytes; not the targets).
    virtual std::size_t videoBytes() const = 0;

    // The last run's results read back (tests and checks only: blocks on the GPU). From the targets
    // where it wrote them.
    std::vector<Vec3f> readPositions();
    std::vector<float> readWidths();
    void readFrames(std::vector<Vec3f>* normals, std::vector<Vec3f>* tangents, std::vector<Vec3f>* bitangents);

    // The kernels: one a line of look_kernels.inc (its entry point NameCS in shaders/look.slang).
    enum class Kernel {
#define GROOMLAB_KERNEL(Name) Name,
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
        Count
    };
    // Buffers by number. kOut0-3: a run's positions and frames, the targets where the kernels write
    // them in place, else the engine's own (copied into the targets after). kBaseWidths: the base's
    // widths per point (the point texture takes them where no Width runs). A base holding hidden
    // core strands (preview_core.h): the run's points and frames in kFull0-3, every strand's, and
    // kShown each shown point's place there, gathered into kOut0-3 (Kernel::Gather). kOutUvs: the
    // uvs target (LookTargets::uvs), bound for the run when the kernels can write it. kPointStrands:
    // each point's strand (strands of different point counts run a thread a point by it).
    // kFieldPrograms: the fields the GPU evaluates (FieldProgram: ops, per-strand values, core
    // strands), kInfluence the guides' influence per strand a running Interpolate writes for them,
    // kLongest the longest core strand's length (a float's bits) FieldCS reads. kPointsA-C: the
    // stages' results between them (C where a Blend's looks part).
    enum Buffer : int {
        kOffsets, kStrandData, kBasePoints, kRamps, kFields, kPointsA, kPointsB, kWidths, kBaseWidths, kOut0, kOut1, kOut2, kOut3,
        kShown, kFull0, kFull1, kFull2, kFull3, kOutUvs, kPointStrands, kFieldPrograms, kInfluence, kLongest, kPointsC, kAuxFirst
    };
    static constexpr int kAuxPerStage = 8;
    static int auxBuffer(std::size_t stage, int which) { return kAuxFirst + static_cast<int>(stage) * kAuxPerStage + which; }
    static constexpr std::size_t kMaxStages = 24;  // (a Wet's levels a stage each)

protected:
    LookEngine() = default;
    struct Binding {
        int in = -1, out[3] = {-1, -1, -1}, aux[3] = {-1, -1, -1};  // aux: gAux0-2 (t5-t7; OpenGL 9, 10, 12)
        bool texture = false;  // the point texture bound as the kernel's image (u3, image binding 11)
        int offsets = -1;      // gOffsets: another set's (a Clump's centers' branch, borrowed), else kOffsets
    };
    // Whether kernel `kernel` is ready to dispatch (`wait`: once it is); throws std::runtime_error
    // when it did not compile. (DirectX 11's always are.)
    virtual bool kernelReady(Kernel kernel, bool wait) {
        (void)kernel;
        (void)wait;
        return true;
    }
    // An engine on the same device (context) sharing these kernels: a Clump's centers' branch runs
    // on one of its own strands (LookCentersRun) before the Clump.
    virtual std::unique_ptr<LookEngine> sibling() = 0;
    // Buffer `buffer` of this engine becomes `from`'s buffer `fromBuffer` (a sibling's: the same
    // backend), for the kernels to read (released as any; the sibling keeps its own).
    virtual void borrow(int buffer, LookEngine& from, int fromBuffer) = 0;
    // The buffer holding every strand's points after the last run (the shown ones and any hidden).
    int allPointsBuffer() const { return allPoints_; }
    // A buffer the kernels read, made (or made again when another size) to hold `data`: writable
    // also lets kernels write it.
    virtual void write(int buffer, const void* data, std::size_t bytes, bool writable) = 0;
    // `bytes` at `offset` of a buffer written before (no larger than it).
    virtual void writeRange(int buffer, std::size_t offset, const void* data, std::size_t bytes) = 0;
    // A buffer the kernels write, at least `bytes` long (its contents undefined).
    virtual void allocate(int buffer, std::size_t bytes) = 0;
    // The base's arrays (points, widths), which the engine holds the base's strands alive for: a
    // copy (write), or on the CPU the very memory (sharesBase: never written, every base shared
    // whole).
    virtual void share(int buffer, const void* data, std::size_t bytes) { write(buffer, data, bytes, false); }
    virtual bool sharesBase() const { return false; }
    // Room for `bytes` in a buffer, filled by the caller before the run (its old contents gone): on
    // the CPU the buffer's own memory; null where the data goes up by write.
    virtual void* writable(int /*buffer*/, std::size_t /*bytes*/) { return nullptr; }
    virtual bool has(int buffer) const = 0;
    virtual void release(int buffer) = 0;
    // A sharing engine's buffer taken out as it is (null: none), and put back.
    virtual std::shared_ptr<void> parkBuffer(int /*buffer*/) { return nullptr; }
    virtual void unparkBuffer(int /*buffer*/, std::shared_ptr<void> /*parked*/) {}
    // The run's outputs: kOut0 (positions) and, with `frames`, kOut1-3 resolved to the targets
    // where the kernels can write them, else to own buffers `bytes` long; the point texture, when
    // one is given, as the image the texture kernel writes (Binding::texture); the uvs target as
    // kOutUvs where the kernels can write it in place (`uvBytes` long at least), else not at all.
    // True when every target buffer given (but the uvs) is written in place.
    virtual bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t uvBytes) = 0;
    // Copies own outputs into the targets the kernels could not write.
    virtual void copyToTargets(bool frames, std::size_t bytes) = 0;
    // `bytes` from byte `fromOffset` of `from` to byte `toOffset` of `to`.
    virtual void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) = 0;
    // The constants of every dispatch of a run, one slot each (look_engine_internal.h
    // StageConstants), uploaded together before the dispatches.
    virtual void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) = 0;
    // groupsX groups a row, groupsY rows (each at most kMaxGroups).
    virtual void dispatch(Kernel kernel, std::size_t slot, const Binding& binding, std::size_t groupsX, std::size_t groupsY) = 0;
    // `count` dispatches of one kernel and binding, their constants slot `slot` then `slot + 1` in
    // turn (a red-black solve's colours), each reading what the one before wrote.
    // Whether a dispatch asked to run only where `buffer`'s first word is at least `least` runs: true
    // where the engine cannot tell without waiting (the GPU's: the kernels find the same either way).
    virtual bool runsIf(int /*buffer*/, std::uint32_t /*least*/) { return true; }
    // A dispatch of `groups` groups whose kernel leaves past `buffer`'s word `word` over `per`: the
    // groups it needs where the engine can tell without waiting (the GPU's: all of them).
    virtual std::size_t countedGroups(int /*buffer*/, std::uint32_t /*word*/, std::uint32_t /*per*/, std::size_t groups) { return groups; }
    virtual void dispatchAlternating(Kernel kernel, std::size_t slot, const Binding& binding, std::size_t groupsX, std::size_t groupsY,
                                     std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) dispatch(kernel, slot + (i & 1), binding, groupsX, groupsY);
    }
    // Runs bracket: the compute state kept and put back; timestamps at the given indices.
    virtual void beginRun() = 0;
    virtual void stamp(std::size_t index) = 0;
    virtual void endRun(std::size_t stamps) = 0;
    // Each stamp's ticks and ticks a second, once the GPU has them; false when not ready (`wait`:
    // blocks), or not timed (another context).
    virtual bool stampTicks(bool wait, std::vector<std::uint64_t>* ticks, double* frequency) = 0;
    virtual std::vector<std::uint8_t> readBack(int buffer, std::size_t bytes) = 0;

    // A thread per point (the strands' point counts the same or not).
    bool perPoint() const { return pointThreads_; }
    // The strands (and their points) a stage runs on: the base's first StageParams::strands up a
    // Merge's input a, else all.
    std::size_t stageStrands(const StageParams& p) const { return p.strands && p.strands < strandCount_ ? p.strands : strandCount_; }
    std::size_t stagePoints(const StageParams& p) const {
        return p.strands && p.strands < strandCount_ && base_ ? base_->strandBegin(p.strands) : pointCount_;
    }

    StrandsPtr base_;
    std::weak_ptr<const StrandSet> heldBase_;  // letGoOfBase's
    std::shared_ptr<const std::vector<std::uint32_t>> view_;  // setBaseView's strands (null: all of base_)
    std::size_t baseStrand(std::size_t i) const { return view_ ? (*view_)[i] : i; }
    std::size_t strandCount_ = 0, pointCount_ = 0, uniformCount_ = 0;  // uniformCount_: points per strand, 0 when they differ
    // The base's shown points' places among its points when it holds hidden strands (else empty),
    // and the points a run's results hold (the shown ones, else all).
    std::vector<std::uint32_t> shownPoints_;
    std::size_t outPoints_ = 0;
    bool inPlace_ = false;
    bool uvsWritten_ = false;

private:
    void prepare(const LookRun& run, bool frames);
    // `frames`: the run makes the lines' frames (they read the root normals).
    void putStrandData(const LookRun& run, bool frames);
    void putFields(const LookFields& fields, const std::vector<StageParams>& stages);
    void putStageInputs(const LookRun& run);
    // The shapers reading their neighbours or a reference: their inputs up, and their dispatch's
    // kernel, bindings (beside `input`, `input2`: a stage's result a RestoreLength reads its
    // reference from) and groups.
    void putShaperInputs(std::size_t k, const StageParams& stage, const StageInputs* in);
    Kernel shaperKernel(std::size_t k, const StageParams& stage, const StageInputs* in, int input2, Binding* b, std::size_t* groups,
                        StageConstants* c) const;
    // Interpolate (stage k) keeping clearance: whether the run walks only the strands bound to the
    // guides it changed (StageHeld::walkPartial), each guide's flag written beside its row.
    void walkOnlyChanged(std::size_t k, const StageParams& params, const InterpolateInputs& data);

    std::uint64_t baseSerial_ = 0;  // each upload of other strands (not their points alone: setBase)
    std::uint64_t shownKey_ = 0;    // what kShown holds (0: nothing)
    void findShownPoints();
    // The base's root normals on its scalp (empty until made: makeRootNormals).
    void makeRootNormals(const StrandSet& s, const ScalpMesh* scalp);
    const ScalpMesh* normalsScalp_ = nullptr;
    std::vector<Vec3f> rootNormals_;
    std::vector<std::uint8_t> rootsOnScalp_;  // whether each root is on the scalp (its normal the skin's)
    // What the buffers hold: the per-strand numbers' key, the fields' serial, each stage's inputs.
    std::vector<std::uint64_t> dataKey_;
    std::vector<std::size_t> dataBlocks_;  // per stage: its block in kStrandData (0: none)
    std::vector<std::size_t> directionBlocks_;  // per stage: its strands' directions' block (StageInputs::directions; 0: none)
    std::uint64_t fieldSerial_ = 0;
    // Per stage kFieldSlots fields (0 and 1, then its affect), each its mode and offset.
    static constexpr std::size_t kFieldSlots = 3;
    std::vector<std::uint32_t> fieldLayout_;
    bool fieldsEmpty_ = false;                // kFields holds no values (none mapped)
    // The fields the GPU evaluates (FieldProgram), per stage and field: where kFieldPrograms holds
    // each one's ops, values and core strands (words), and how it reads.
    struct ProgramAt {
        bool on = false;
        std::uint32_t ops = 0, opCount = 0, statics = 0, slotCount = 0, core = 0, coreCount = 0, depth = 0;
        bool shape = false, longest = false, influence = false, normal = false;
        float low = 0.0f, high = 1.0f;
    };
    std::vector<ProgramAt> programs_;  // kFieldSlots a stage
    struct StageHeld {
        int kind = -1;            // the stage's (StageKind): another kind at this index holds nothing
        std::shared_ptr<const void> keep;  // Interpolate: its binding, alive (its chunks are compared by address)
        const void* a = nullptr;  // Interpolate: binding; Clump: assigned; Curl: clumpOf; Part: sides; Collide: the scalp's tables
        const void* b = nullptr;  // Interpolate: rows; Clump: curves (fixed) or chosen; Curl: members; Collide: the collider's
        const void* c = nullptr;  // Clump: its variation; Curl: its clumps' numbers
        const void* d = nullptr;  // Clump: its neighbors
        std::uint32_t aux[4] = {};
        std::shared_ptr<const void> alive[4];  // what a, b, c and d are of where nothing else keeps it (compared by address)
        bool framesMade = false;
        std::uint64_t madeFrames = 0;  // a sharing engine's own root frames: for the strands of this baseSerial_ (0: none)
        std::vector<const void*> chunks;  // Interpolate: the binding's chunks its buffer holds
        // Interpolate: where each chunk's records are (words) and the room there, where the records
        // end and the buffer's room for them (a chunk bound again that outgrows its room goes last).
        std::vector<std::uint32_t> chunkAt, chunkRoom;
        std::size_t recordsEnd = 0, recordsRoom = 0;
        std::shared_ptr<const void> tables;  // Interpolate: the clearance field's tables its buffer holds
        std::shared_ptr<const void> walk;    // Interpolate: a deformed scalp's walk words its buffer holds after them
        // Interpolate keeping clearance: every strand's walked points (its aux buffer 4) as the last
        // run left them, from these guides and what walkKey hashes (the base, binding, field and
        // params). A run that changes nothing else walks only the strands bound to a guide it
        // changed (a brush dab's), the others' points taken from there (ClearanceCS).
        std::shared_ptr<const InterpolateGuideRows> walkRows;
        std::uint64_t walkKey = 0;
        std::size_t walkPoints = 0;
        bool walked = false;       // the buffer holds the walk of walkRows and walkKey
        bool walkPartial = false;  // this run walks only the strands of the guides it changed
        // SelfCollide and Static: the room the hair grid's buffers hold (block cells along x, y, z;
        // blocks), and where the scalp's walls start in its floor's tables' buffer (aux 4; ~0: none).
        std::uint32_t room[4] = {};
        std::uint32_t wallsAt = 0xFFFFFFFFu;
    };
    std::vector<StageHeld> held_;
    // A sharing engine (the CPU's, whose runs are an operator's one stage each) keeps what a stage
    // slot held for each kind that took it: another kind's turn parks it (its held state, its aux
    // buffers as they are), the kind's next takes it back (parkBuffer, unparkBuffer: no copy).
    struct Parked {
        StageHeld held;
        std::shared_ptr<void> buffers[kAuxPerStage];
    };
    std::map<std::pair<std::size_t, int>, Parked> parked_;
    // Per stage: the sibling running a Clump's centers' branch or a Merge's input b's run (null: none),
    // and the last runs'.
    std::vector<std::unique_ptr<LookEngine>> centers_;
    // Stage k's sibling (made at its first), `branch` run on it before this engine's run (the same
    // queue).
    LookEngine& runSibling(std::size_t k, const LookCentersRun& branch);
    void runCenters(std::size_t k, const LookCentersRun& run);
    // A Merge's input b's run (stage k; b's strands after the first `strandsA`): its points (aux 0)
    // and, where it makes widths, its widths (aux 1) borrowed for the Merge.
    void runMerged(std::size_t k, std::size_t strandsA, const LookCentersRun& run);
    // The run up a Cull's, Split's or Mirror's input (stage k): its points (aux 0) and, where it makes
    // widths, its widths (aux 1) borrowed for the stage, which lays them out (aux 2: GatherInputs).
    void runGathered(std::size_t k, const LookCentersRun& run, const GatherInputs& layout);
    int allPoints_ = -1;
    std::vector<float> ramps_;
    // The last run, for timings and read-backs.
    struct Stamped {
        int kind;  // StageKind, or -1 lines, -2 copy, -4 the point texture
        std::size_t from, to;
    };
    std::vector<Stamped> stamped_;
    std::size_t stampCount_ = 0;
    bool stampsPending_ = false;
    GpuTimes times_;
    int resultBuffer_ = -1;
    bool framesRun_ = false, widthsRun_ = false;
    bool pointThreads_ = true;
    std::size_t dispatchLimit_ = kMaxGroups;
    std::uint64_t pointStrandsKey_ = 0;  // the base upload kPointStrands was made for (0: none)
    double prepareParts_[3] = {};
    double uploadMs_ = 0, uploadMegabytes_ = 0, runUploadMs_ = 0, runUploadMegabytes_ = 0, fieldMegabytes_ = 0;
    std::size_t uploads_ = 0, uploadStrands_ = 0;
    bool uploadPartial_ = false;
    WalkStats lastWalk_;
};

// The engine on the CPU: look.slang's kernels compiled to C++ (the GPU's own source; the core's
// operators' bits), its buffers in memory, each dispatch's groups across the core's threads; a
// host's targets are arrays in memory, written in place.
std::unique_ptr<LookEngine> makeCpuLookEngine();
}  // namespace groomlab::gpu
