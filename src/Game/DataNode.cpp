#include <cstring>
#include "generated/band3_init.h"
#include "src/Game/DataNode.h"
#include "src/Game/DataArray.h"
#include "src/Game/song_id.h"

extern "C" void __imp__DataNode__Evaluate(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DataNode__UseQueue(PPCContext& ctx, uint8_t* base);
extern "C" void Symbol__Symbol(PPCContext& ctx, uint8_t* base);
extern "C" void ObjectDir__FindObject(PPCContext& ctx, uint8_t* base);

constexpr uint32_t gEvalIndex_addr = 0x82E05220;
constexpr uint32_t gEvalNode_addr  = 0x82E05240;
constexpr uint32_t gDataDir_addr   = 0x82E05DB0;

// the return address after SongMetadata's constructor's call reading song_id
constexpr uint32_t kSongMetadataSongIdCall = 0x827AA7D8;

extern "C" void DataNode__Evaluate(PPCContext& ctx, uint8_t* base);

// --- helper cruft ---

// helpers to cast guest pointers to structures
static inline const band3::DataNode* node(uint8_t* base, uint32_t addr) {
    return reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(addr));
}
static inline const band3::DataArray* array(uint8_t* base, uint32_t addr) {
    return reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(addr));
}

// calls evaluate, returns the pointer
static inline uint32_t evaluate(PPCContext& ctx, uint8_t* base) {
    DataNode__Evaluate(ctx, base);
    return ctx.r3.u32;
}

static inline uint32_t literal_str(uint8_t* base, uint32_t addr) {
    auto* n = node(base, addr);
    if (n->type == band3::kDataSymbol)
        return n->value;
    return array(base, n->value)->mNodes;
}

// --- DataNode functions ---

// DataNode* DataNode::Var() const
// Retail folds this with DataArrayPtr::operator DataArray*: both are lwz r3,0(r3); blr.
extern "C" REX_FUNC(DataNode__Var) {
    ctx.r3.u64 = node(base, ctx.r3.u32)->value;
}

// int DataNode::Int(const DataArray*) const
extern "C" REX_FUNC(DataNode__Int) {
    // read before evaluating: evaluating can run guest code that overwrites lr
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    auto* n = node(base, evaluate(ctx, base));
    // SongMetadata's constructor reading song_id: text there gets RB3Enhanced's number
    if (caller == kSongMetadataSongIdCall &&
        (n->type == band3::kDataSymbol || n->type == band3::kDataString)) {
        const char* text = reinterpret_cast<const char*>(REX_RAW_ADDR(literal_str(base, ctx.r3.u32)));
        const int32_t id = band3::CorrectedSongId(text);
        band3::LogSongIdCorrection(text, id);
        ctx.r3.u64 = static_cast<uint64_t>(static_cast<int64_t>(id));
        return;
    }
    ctx.r3.u64 = n->value;
}

// const char* DataNode::LiteralStr(const DataArray*) const
extern "C" REX_FUNC(DataNode__LiteralStr) {
    ctx.r3.u64 = literal_str(base, ctx.r3.u32);
}

// Symbol DataNode::Sym(const DataArray*) const
extern "C" REX_FUNC(DataNode__Sym) {
    uint32_t out = ctx.r3.u32;
    ctx.r3.u64 = ctx.r4.u64;
    uint32_t val = node(base, evaluate(ctx, base))->value;
    REX_STORE_U32(out, val);
    ctx.r3.u64 = out;
}

// const char* DataNode::Str(const DataArray*) const
extern "C" REX_FUNC(DataNode__Str) {
    ctx.r3.u64 = literal_str(base, evaluate(ctx, base));
}

// float DataNode::Float(const DataArray*) const
extern "C" REX_FUNC(DataNode__Float) {
    auto* n = node(base, evaluate(ctx, base));
    if (n->type == band3::kDataInt) {
        ctx.f1.f64 = static_cast<double>(static_cast<int32_t>(n->value));
    } else {
        uint32_t raw = n->value;
        float f;
        std::memcpy(&f, &raw, sizeof(float));
        ctx.f1.f64 = static_cast<double>(f);
    }
}

// Symbol DataNode::ForceSym(const DataArray*) const
extern "C" REX_FUNC(DataNode__ForceSym) {
    uint32_t out = ctx.r3.u32;
    ctx.r3.u64 = ctx.r4.u64;
    auto* n = node(base, evaluate(ctx, base));
    if (n->type == band3::kDataSymbol) {
        REX_STORE_U32(out, n->value);
        ctx.r3.u64 = out;
    } else {
        ctx.r3.u64 = out;
        ctx.r4.u64 = array(base, n->value)->mNodes;
        Symbol__Symbol(ctx, base);
    }
}

// const DataNode& DataNode::Evaluate() const
extern "C" REX_FUNC(DataNode__Evaluate) {
    auto* n = node(base, ctx.r3.u32);
    if (n->type == band3::kDataVar) {
        ctx.r3.u64 = n->value;
    } else if (n->type == band3::kDataCommand || n->type == band3::kDataProperty) {
        __imp__DataNode__Evaluate(ctx, base);
    }
}

// const DataNode& UseQueue(const DataNode& node)
extern "C" REX_FUNC(DataNode__UseQueue) {
    uint32_t src = ctx.r3.u32;
    uint32_t idx = REX_LOAD_U32(gEvalIndex_addr);
    uint32_t dst = gEvalNode_addr + idx * 8;

    auto* old_n = node(base, dst);
    auto* new_n = node(base, src);

    if ((old_n->type | new_n->type) & band3::kDataArray) {
        __imp__DataNode__UseQueue(ctx, base);
        return;
    }

    REX_STORE_U32(dst + 0, new_n->value);
    REX_STORE_U32(dst + 4, new_n->type);
    REX_STORE_U32(gEvalIndex_addr, (idx + 1) & 7);
    ctx.r3.u64 = dst;
}

// bool DataNode::NotNull() const
extern "C" REX_FUNC(DataNode__NotNull) {
    uint32_t addr = evaluate(ctx, base);
    auto* n = node(base, addr);

    if (n->type == band3::kDataSymbol) {
        ctx.r3.u64 = (*REX_RAW_ADDR(n->value) != 0) ? 1 : 0;
    } else if (n->type == band3::kDataString) {
        auto* arr = array(base, n->value);
        ctx.r3.u64 = ((short)arr->mSize < -1) ? 1 : 0;
    } else if (n->type == band3::kDataGlob) {
        auto* arr = array(base, n->value);
        ctx.r3.u64 = ((unsigned short)arr->mSize != 0) ? 1 : 0;
    } else {
        ctx.r3.u64 = (n->value != 0) ? 1 : 0;
    }
}

// Object* DataNode::GetObj(const DataArray*) const
extern "C" REX_FUNC(DataNode__GetObj) {
    uint32_t addr = evaluate(ctx, base);
    auto* n = node(base, addr);

    if (n->type == band3::kDataObject) {
        ctx.r3.u64 = n->value;
    } else {
        uint32_t str = literal_str(base, addr);
        if (*REX_RAW_ADDR(str) != '\0') {
            ctx.r3.u64 = REX_LOAD_U32(gDataDir_addr);
            ctx.r4.u64 = str;
            ctx.r5.u64 = 1;
            ObjectDir__FindObject(ctx, base);
        } else {
            ctx.r3.u64 = 0;
        }
    }
}
