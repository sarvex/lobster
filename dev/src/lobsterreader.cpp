// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lobster/stdafx.h"

#include "lobster/natreg.h"

#include "lobster/lex.h"

#include "flatbuffers/idl.h"

#define FLATBUFFERS_DEBUG_VERIFICATION_FAILURE
#include "lobster/bytecode_generated.h"

namespace lobster {

struct Deserializer {
    VM &vm;
    vector<Value> stack;
    vector<bool> is_ref;

    ~Deserializer() {
        assert(stack.size() == is_ref.size());
        for (size_t i = 0; i < stack.size(); i++) {
            if (is_ref[i]) stack[i].ref()->Dec(vm);
        }
    }

    void PushV(Value v, bool ir = false) {
        stack.emplace_back(v);
        is_ref.push_back(ir);
    }

    Value PopV() {
        auto v = stack.back();
        stack.pop_back();
        is_ref.pop_back();
        return v;
    }

    void PopVN(size_t len) {
        stack.resize(stack.size() - len);
        is_ref.resize(is_ref.size() - len);
    }

    Deserializer(VM &vm) : vm(vm) {
        stack.reserve(16);
        is_ref.reserve(16);
    }

    bool PushDefault(type_elem_t typeoff, int defval) {
        auto &ti = vm.GetTypeInfo(typeoff);
        switch (ti.t) {
            case V_INT:
                PushV(defval);
                break;
            case V_FLOAT:
                PushV(int2float(defval).f);
                break;
            case V_NIL:
                PushV(NilVal());
                break;
            case V_VECTOR:
                PushV(vm.NewVec(0, 0, typeoff), true);
                break;
            case V_STRUCT_S:
            case V_STRUCT_R:
            case V_CLASS: {
                for (int i = 0; i < ti.len; i++) {
                    PushDefault(ti.elemtypes[i].type, ti.elemtypes[i].defval);
                }
                if (ti.t == V_CLASS) {
                    auto vec = vm.NewObject(ti.len, typeoff);
                    if (ti.len) vec->CopyElemsShallow(&stack[stack.size() - (size_t)ti.len], ti.len);
                    PopVN(ti.len);
                    PushV(vec, true);
                }
                break;
            }
            default:
                return false;
        }
        return true;
    }

    pair<const TypeInfo *, type_elem_t> LookupSubClass(string_view sname,
            const TypeInfo *ti, type_elem_t typeoff) {
        // Attempt to find this a subsclass.
        // TODO: subclass of subclass, etc?
        vm.EnsureUDTLookupPopulated();
        auto &udts = vm.UDTLookup[sname];
        for (auto udt : udts) {
            if (udt->super_idx() == ti->structidx) {
                // Note: this field only not -1 for UDTs actually constructed/used.
                typeoff = (type_elem_t)udt->typeidx();
                if (typeoff >= 0) {
                    return { &vm.GetTypeInfo(typeoff), typeoff };
                }
            }
        }
        return { nullptr, (type_elem_t)-1 };
    }
};

struct ValueParser : Deserializer {
    vector<pair<string, string>> filenames;
    Lex lex;

    ValueParser(VM &vm, string_view _src) : Deserializer(vm), lex("string", filenames, _src) {
        lex.do_string_interpolation = false;
    }

    void Parse(StackPtr &sp, type_elem_t typeoff) {
        ParseFactor(typeoff, true);
        Gobble(T_LINEFEED);
        Expect(T_ENDOFFILE);
        assert(stack.size() == 1);
        Push(sp, PopV());
    }

    // Vector or struct.
    void ParseElems(TType end, type_elem_t typeoff, iint numelems, bool push) {
        Gobble(T_LINEFEED);
        auto &ti = vm.GetTypeInfo(typeoff);
        auto stack_start = stack.size();
        auto NumElems = [&]() { return iint(stack.size() - stack_start); };
        if (lex.token == end) lex.Next();
        else {
            for (;;) {
                if (NumElems() == numelems) {
                    ParseFactor(TYPE_ELEM_ANY, false);
                } else {
                    auto eti = ti.t == V_VECTOR ? ti.subt : ti.GetElemOrParent(NumElems());
                    ParseFactor(eti, push);
                }
                bool haslf = lex.token == T_LINEFEED;
                if (haslf) lex.Next();
                if (lex.token == end) break;
                if (!haslf) Expect(T_COMMA);
            }
            lex.Next();
        }
        if (!push) return;
        if (numelems >= 0) {
            while (NumElems() < numelems) {
                if (!PushDefault(ti.elemtypes[NumElems()].type, ti.elemtypes[NumElems()].defval))
                    lex.Error("no default value exists for missing struct elements");
            }
        }
        if (ti.t == V_CLASS) {
            auto len = NumElems();
            auto vec = vm.NewObject(len, typeoff);
            if (len) vec->CopyElemsShallow(stack.size() - len + stack.data(), len);
            PopVN(len);
            PushV(vec, true);
        } else if (ti.t == V_VECTOR) {
            auto &sti = vm.GetTypeInfo(ti.subt);
            auto width = IsStruct(sti.t) ? sti.len : 1;
            auto len = NumElems();
            auto n = len / width;
            auto vec = vm.NewVec(n, n, typeoff);
            if (len) vec->CopyElemsShallow(stack.size() - len + stack.data());
            PopVN(len);
            PushV(vec, true);
        }
        // else if ti.t == V_STRUCT_* then.. do nothing!
    }

    void ExpectType(ValueType given, ValueType needed) {
        if (given != needed && needed != V_ANY) {
            lex.Error("type " +
                      BaseTypeName(needed) +
                      " required, " +
                      BaseTypeName(given) +
                      " given");
        }
    }

    void ParseFactor(type_elem_t typeoff, bool push) {
        auto ti = &vm.GetTypeInfo(typeoff);
        if (ti->t == V_NIL && lex.token != T_NIL) {
            ti = &vm.GetTypeInfo(typeoff = ti->subt);
        }
        auto vt = ti->t;
        switch (lex.token) {
            case T_INT: {
                ExpectType(V_INT, vt);
                auto i = lex.ival;
                lex.Next();
                if (push) PushV(i);
                break;
            }
            case T_FLOAT: {
                ExpectType(V_FLOAT, vt);
                auto f = lex.fval;
                lex.Next();
                if (push) PushV(f);
                break;
            }
            case T_STR: {
                ExpectType(V_STRING, vt);
                string s = std::move(lex.sval);
                lex.Next();
                if (push) {
                    auto str = vm.NewString(s);
                    PushV(str, true);
                }
                break;
            }
            case T_NIL: {
                ExpectType(V_NIL, vt);
                lex.Next();
                if (push) PushV(NilVal());
                break;
            }
            case T_MINUS: {
                lex.Next();
                ParseFactor(typeoff, push);
                if (push) {
                    switch (typeoff) {
                        case TYPE_ELEM_INT:   stack.back().setival(stack.back().ival() * -1); break;
                        case TYPE_ELEM_FLOAT: stack.back().setfval(stack.back().fval() * -1); break;
                        default: lex.Error("unary minus: numeric value expected");
                    }
                }
                break;
            }
            case T_LEFTBRACKET: {
                ExpectType(V_VECTOR, vt);
                lex.Next();
                ParseElems(T_RIGHTBRACKET, typeoff, -1, push);
                break;
            }
            case T_IDENT: {
                if (vt == V_INT && ti->enumidx >= 0) {
                    auto opt = vm.LookupEnum(lex.sattr, ti->enumidx);
                    if (!opt) lex.Error("unknown enum value " + lex.sattr);
                    lex.Next();
                    if (push) PushV(*opt);
                    break;
                }
                if (!IsUDT(vt) && vt != V_ANY)
                    lex.Error("class/struct type required, " + BaseTypeName(vt) + " given");
                auto sname = lex.sattr;
                lex.Next();
                Expect(T_LEFTCURLY);
                auto name = vm.StructName(*ti);
                if (name != sname) {
                    auto p = LookupSubClass(sname, ti, typeoff);
                    if (!p.first)
                        lex.Error("class/struct type " + name + " required, " + sname + " given");
                    ti = p.first;
                    typeoff = p.second;
                }
                ParseElems(T_RIGHTCURLY, typeoff, ti->len, push);
                break;
            }
            default:
                lex.Error("illegal start of expression: " + lex.TokStr());
                PushV(NilVal());
                break;
        }
    }

    void Expect(TType t) {
        if (lex.token != t)
            lex.Error(lex.TokStr(t) + " expected, found: " + lex.TokStr());
        lex.Next();
    }

    void Gobble(TType t) {
        if (lex.token == t) lex.Next();
    }
};

static void ParseData(StackPtr &sp, VM &vm, type_elem_t typeoff, string_view inp) {
    ValueParser parser(vm, inp);
    #ifdef USE_EXCEPTION_HANDLING
    try
    #endif
    {
        parser.Parse(sp, typeoff);
        Push(sp, NilVal());
    }
    #ifdef USE_EXCEPTION_HANDLING
    catch (string &s) {
        Push(sp, NilVal());
        Push(sp, vm.NewString(s));
    }
    #endif
}


struct FlexBufferParser : Deserializer {

    FlexBufferParser(VM &vm) : Deserializer(vm) {}

    void Parse(StackPtr &sp, type_elem_t typeoff, flexbuffers::Reference r) {
        ParseFactor(r, typeoff);
        assert(stack.size() == 1);
        Push(sp, PopV());
    }

    void Error(const string &s) {
        // FIXME: not great on non-exception platforms, this should not abort.
        THROW_OR_ABORT(cat("flexbuffers_binary_to_value: ", s));
    }

    void ExpectType(ValueType given, ValueType needed) {
        if (given != needed && needed != V_ANY) {
            Error(cat("type ", BaseTypeName(needed), " required, ", BaseTypeName(given),
                               " given"));
        }
    }

    void ParseFactor(flexbuffers::Reference r, type_elem_t typeoff) {
        auto ti = &vm.GetTypeInfo(typeoff);
        auto ft = r.GetType();
        if (ti->t == V_NIL && ft != flexbuffers::FBT_NULL) {
            ti = &vm.GetTypeInfo(typeoff = ti->subt);
        }
        auto vt = ti->t;
        switch (ft) {
            case flexbuffers::FBT_INT:
            case flexbuffers::FBT_BOOL: {
                ExpectType(V_INT, vt);
                PushV(r.AsInt64());
                break;
            }
            case flexbuffers::FBT_FLOAT: {
                ExpectType(V_FLOAT, vt);
                PushV(r.AsDouble());
                break;
            }
            case flexbuffers::FBT_STRING: {
                ExpectType(V_STRING, vt);
                auto s = r.AsString();
                auto str = vm.NewString(string_view(s.c_str(), s.size()));
                PushV(str, true);
                break;
            }
            case flexbuffers::FBT_NULL: {
                ExpectType(V_NIL, vt);
                PushV(NilVal());
                break;
            }
            case flexbuffers::FBT_VECTOR: {
                ExpectType(V_VECTOR, vt);
                auto v = r.AsVector();
                auto stack_start = stack.size();
                for (size_t i = 0; i < v.size(); i++) {
                    ParseFactor(v[i], ti->subt);
                }
                auto &sti = vm.GetTypeInfo(ti->subt);
                auto width = IsStruct(sti.t) ? sti.len : 1;
                auto len = iint(stack.size() - stack_start);
                auto n = len / width;
                auto vec = vm.NewVec(n, n, typeoff);
                if (len) vec->CopyElemsShallow(stack.size() - len + stack.data());
                PopVN(len);
                PushV(vec, true);
                break;
            }
            case flexbuffers::FBT_MAP: {
                if (!IsUDT(vt) && vt != V_ANY)
                    Error(cat("class/struct type required, ", BaseTypeName(vt), " given"));
                auto m = r.AsMap();
                auto name = vm.StructName(*ti);
                auto sname = m["_type"];
                if (sname.IsString() && sname.AsString().c_str() != name) {
                    auto p = LookupSubClass(sname.AsString().c_str(), ti, typeoff);
                    if (!p.first)
                        Error(cat("class/struct type ", name, " required, ", sname.AsString().str(),
                                  " given"));
                    ti = p.first;
                    typeoff = p.second;
                }
                auto stack_start = stack.size();
                auto NumElems = [&]() { return iint(stack.size() - stack_start); };
                for (int i = 0; NumElems() != ti->len; i++) {
                    auto fname = vm.LookupField(ti->structidx, i);
                    auto eti = ti->GetElemOrParent(NumElems());
                    auto e = m[fname.data()];
                    if (e.IsNull()) {
                        if(!PushDefault(eti, ti->elemtypes[NumElems()].defval))
                            Error("no default value exists for missing field " + fname);
                    } else {
                        ParseFactor(e, eti);
                    }
                }
                if (vt == V_CLASS) {
                    auto len = NumElems();
                    auto vec = vm.NewObject(len, typeoff);
                    if (len) vec->CopyElemsShallow(stack.size() - len + stack.data(), len);
                    PopVN(len);
                    PushV(vec, true);
                }
                // else if vt == V_STRUCT_* then.. do nothing!
                break;
            }
            default:
                Error("can\'t convert to value: " + r.ToString());
                PushV(NilVal());
                break;
        }
    }
};

static void ParseFlexData(StackPtr &sp, VM &vm, type_elem_t typeoff, flexbuffers::Reference r) {
    FlexBufferParser parser(vm);
    #ifdef USE_EXCEPTION_HANDLING
    try
    #endif
    {
        parser.Parse(sp, typeoff, r);
        Push(sp, NilVal());
    }
    #ifdef USE_EXCEPTION_HANDLING
    catch (string &s) {
        Push(sp, NilVal());
        Push(sp, vm.NewString(s));
    }
    #endif
}

struct LobsterBinaryParser : Deserializer {

    LobsterBinaryParser(VM &vm) : Deserializer(vm) {}

    void Parse(StackPtr &sp, type_elem_t typeoff, const uint8_t *data, const uint8_t *end) {
        ParseElem(data, end, typeoff);
        assert(stack.size() == 1);
        Push(sp, PopV());
    }

    void Error(const string &s) {
        // FIXME: not great on non-exception platforms, this should not abort.
        THROW_OR_ABORT(cat("lobster_binary_to_value: ", s));
    }

    void Truncated() {
        Error("data truncated");
    }

    void ParseElem(const uint8_t *&data, const uint8_t *end, type_elem_t typeoff) {
        auto base_ti = &vm.GetTypeInfo(typeoff);
        auto ti = base_ti;
        if (ti->t == V_NIL) {
            ti = &vm.GetTypeInfo(typeoff = ti->subt);
        }
        if (end == data) Truncated();
        switch (ti->t) {
            case V_INT: {
                PushV(DecodeVarintS(data, end));
                break;
            }
            case V_FLOAT: {
                float f;
                if (end - data < (ptrdiff_t)sizeof(float)) Truncated();
                memcpy(&f, data, sizeof(float));
                data += sizeof(float);
                PushV(f);
                break;
            }
            case V_STRING: {
                auto len = DecodeVarintU(data, end);
                if (!len && base_ti->t == V_NIL) {
                    PushV(NilVal());
                } else {
                    auto str = vm.NewString(string_view((const char *)data, (size_t)len));
                    data += len;
                    PushV(str, true);
                }
                break;
            }
            case V_VECTOR: {
                auto len = DecodeVarintU(data, end);
                if (!len && base_ti->t == V_NIL) {
                    PushV(NilVal());
                } else {
                    auto stack_start = stack.size();
                    for (size_t i = 0; i < len; i++) {
                        ParseElem(data, end, ti->subt);
                    }
                    auto &sti = vm.GetTypeInfo(ti->subt);
                    auto width = IsStruct(sti.t) ? sti.len : 1;
                    auto len = iint(stack.size() - stack_start);
                    auto n = len / width;
                    auto vec = vm.NewVec(n, n, typeoff);
                    if (len) vec->CopyElemsShallow(stack.size() - len + stack.data());
                    PopVN(len);
                    PushV(vec, true);
                }
                break;
            }
            case V_CLASS: {
                auto elen = (int)DecodeVarintU(data, end);
                if (!elen && base_ti->t == V_NIL) {
                    PushV(NilVal());
                } else {
                    auto ser_id = DecodeVarintU(data, end);
                    typeoff = vm.GetSubClassFromSerID(typeoff, (uint32_t)ser_id);
                    if (typeoff < 0)
                        Error(cat("serialization id ", ser_id, " is not a sub-class of ",
                                  vm.StructName(*ti)));
                    ti = &vm.GetTypeInfo(typeoff);
                    auto stack_start = stack.size();
                    auto NumElems = [&]() { return iint(stack.size() - stack_start); };
                    for (int i = 0; NumElems() != ti->len; i++) {
                        auto eti = ti->GetElemOrParent(NumElems());
                        if (NumElems() >= elen) {
                            if (!PushDefault(eti, ti->elemtypes[NumElems()].defval))
                                Error("no default value exists for missing field " +
                                      vm.LookupField(ti->structidx, i));
                        } else {
                            ParseElem(data, end, eti);
                        }
                    }
                    if (elen > NumElems()) {
                        // We have fields from a future version of this class, sadly we don't
                        // know how to read past these fields since we have no type data.
                        Error("extra fields presents in " + vm.StructName(*ti));
                    }
                    auto len = NumElems();
                    auto vec = vm.NewObject(len, typeoff);
                    if (len) vec->CopyElemsShallow(stack.size() - len + stack.data(), len);
                    PopVN(len);
                    PushV(vec, true);
                }
                break;
            }
            case V_STRUCT_S:
            case V_STRUCT_R: {
                auto stack_start = stack.size();
                auto NumElems = [&]() { return iint(stack.size() - stack_start); };
                // NOTE: this provides no protection against structs changing in size,
                // unlike classes. It will simply parse wrong.
                while (NumElems() != ti->len) {
                    auto eti = ti->GetElemOrParent(NumElems());
                    ParseElem(data, end, eti);
                }
                break;
            }
            default:
                Error("can\'t convert to value: " + ti->Debug(vm, false));
                PushV(NilVal());
                break;
        }
    }
};

static void ParseLobsterBinaryData(StackPtr &sp, VM &vm, type_elem_t typeoff, const uint8_t *data,
                                   size_t size) {
    LobsterBinaryParser parser(vm);
    #ifdef USE_EXCEPTION_HANDLING
    try
    #endif
    {
        parser.Parse(sp, typeoff, data, data + size);
        Push(sp, NilVal());
    }
    #ifdef USE_EXCEPTION_HANDLING
    catch (string &s) {
        Push(sp, NilVal());
        Push(sp, vm.NewString(s));
    }
    #endif
}

void AddReader(NativeRegistry &nfr) {

nfr("parse_data", "typeid,stringdata", "TS", "A1?S?",
    "parses a string containing a data structure in lobster syntax (what you get if you convert"
    " an arbitrary data structure to a string) back into a data structure. supports"
    " int/float/string/vector and classes. classes will be forced to be compatible with their "
    " current definitions, i.e. too many elements will be truncated, missing elements will be"
    " set to 0/nil if possible. useful for simple file formats. returns the value and an error"
    " string as second return value (or nil if no error)",
    [](StackPtr &sp, VM &vm) {
        auto ins = Pop(sp).sval();
        auto type = Pop(sp).ival();
        ParseData(sp, vm, (type_elem_t)type, ins->strv());
    });

nfr("flexbuffers_value_to_binary", "val,max_nesting,cycle_detection", "AI?B?", "S",
    "turns any reference value into a flexbuffer. max_nesting defaults to 100. "
    "cycle_detection is by default off (expensive)",
    [](StackPtr &, VM &vm, Value &val, Value &maxnest, Value &cycle_detect) {
        ToFlexBufferContext fbc(vm);
        auto mn = maxnest.ival();
        if (mn > 0) fbc.max_depth = mn;
        fbc.cycle_detect = cycle_detect.True();
        val.ToFlexBuffer(fbc, val.refnil() ? val.refnil()->ti(vm).t : V_NIL, {}, -1);
        fbc.builder.Finish();
        if (!fbc.cycle_hit.empty())
            vm.BuiltinError("flexbuffers_value_to_binary: data structure contains a cycle: " +
                            fbc.cycle_hit);
        if (!fbc.max_depth_hit.empty())
            vm.BuiltinError(
                "flexbuffers_value_to_binary: data structure exceeds max nesting depth: " +
                fbc.max_depth_hit);
        auto s = vm.NewString(
            string_view((const char *)fbc.builder.GetBuffer().data(), fbc.builder.GetSize()));
        return Value(s);
    });

nfr("flexbuffers_binary_to_value", "typeid,flex", "TS", "A1?S?",
    "turns a flexbuffer into a value",
    [](StackPtr &sp, VM &vm) {
        auto fsv = Pop(sp).sval()->strv();
        auto id = Pop(sp).ival();
        vector<uint8_t> reuse_buffer;
        if (flexbuffers::VerifyBuffer((const uint8_t *)fsv.data(), fsv.size(), &reuse_buffer)) {
            auto root = flexbuffers::GetRoot((const uint8_t *)fsv.data(), fsv.size());
            ParseFlexData(sp, vm, (type_elem_t)id, root);
        } else { 
            Push(sp, NilVal());
            Push(sp, vm.NewString("flexbuffer binary does not verify!"));
        }
    });

nfr("flexbuffers_binary_to_json", "flex,field_quotes,indent_string", "SBS", "S?S?",
    "turns a flexbuffer into a JSON string. If indent_string is empty, will be a single line string",
    [](StackPtr &sp, VM &vm) {
        auto indent_string = Pop(sp).sval()->strvnt();
        auto quoted = Pop(sp).ival();
        auto fsv = Pop(sp).sval()->strv();
        vector<uint8_t> reuse_buffer;
        if (flexbuffers::VerifyBuffer((const uint8_t *)fsv.data(), fsv.size(), &reuse_buffer)) {
            auto root = flexbuffers::GetRoot((const uint8_t *)fsv.data(), fsv.size());
            string json;
            root.ToString(true, quoted, json, indent_string.size() != 0, 0, indent_string.c_str());
            auto s = vm.NewString(json);
            Push(sp, s);
            Push(sp, NilVal());
        } else {
            Push(sp, NilVal());
            Push(sp, vm.NewString("flexbuffer binary does not verify!"));
        }
    });

nfr("flexbuffers_json_to_binary", "json", "S", "SS?",
    "turns a JSON string into a flexbuffer, second value is error, if any",
    [](StackPtr &sp, VM &vm, Value &json) {
        flexbuffers::Builder builder;
        flatbuffers::Parser parser;
        auto err = NilVal();
        if (!parser.ParseFlexBuffer(json.sval()->strv().data(), "(flexbuffers_json_to_binary)",
                                    &builder)) {
            err = vm.NewString(parser.error_);
            Push(sp, vm.NewString(""));
        } else {
            Push(sp, vm.NewString(
                string_view((const char *)builder.GetBuffer().data(), builder.GetSize())));
        }
        return err;
    });

nfr("lobster_value_to_binary", "val", "A", "S",
    "turns any reference value into a binary using a fast & compact Lobster native serialization format. "
    "this is intended for threads/networking, not for storage (since it is not readable by other languages). "
    "data structures participating must have been marked by attribute serializable. "
    "does not provide protection against cycles, use flexbuffers if that is a concern. ",
    [](StackPtr &, VM &vm, Value &val) {
        vector<uint8_t> buf;
        val.ToLobsterBinary(vm, buf, val.refnil() ? val.refnil()->ti(vm).t : V_NIL);
        // FIXME: since this is meant to be fast, worth seeing if this can be made 0-copy?
        auto s = vm.NewString(
            string_view((const char *)buf.data(), buf.size()));
        return Value(s);
    });

nfr("lobster_binary_to_value", "typeid,bin", "TS", "A1?S?",
    "turns binary created by lobster_value_to_binary back into a value",
    [](StackPtr &sp, VM &vm) {
        auto fsv = Pop(sp).sval()->strv();
        auto id = Pop(sp).ival();
        ParseLobsterBinaryData(sp, vm, (type_elem_t)id, (const uint8_t *)fsv.data(), fsv.size());
    });
}

}
