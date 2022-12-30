#include "kqp_query_data.h"

#include <ydb/core/protos/kqp_physical.pb.h>
#include <ydb/library/mkql_proto/mkql_proto.h>
#include <ydb/library/yql/minikql/mkql_string_util.h>
#include <ydb/library/yql/utils/yql_panic.h>
#include <ydb/library/yql/dq/runtime/dq_transport.h>
#include <ydb/library/yql/public/udf/udf_data_type.h>

namespace NKikimr::NKqp {

using namespace NKikimr::NMiniKQL;
using namespace NYql;
using namespace NYql::NUdf;

TTxAllocatorState::TTxAllocatorState(const IFunctionRegistry* functionRegistry,
    TIntrusivePtr<ITimeProvider> timeProvider, TIntrusivePtr<IRandomProvider> randomProvider)
    : Alloc(__LOCATION__, NKikimr::TAlignedPagePoolCounters(), functionRegistry->SupportsSizedAllocators())
    , TypeEnv(Alloc)
    , MemInfo("TQueryData")
    , HolderFactory(Alloc.Ref(), MemInfo, functionRegistry)
{
    Alloc.Release();
    TimeProvider = timeProvider;
    RandomProvider = randomProvider;
}

TTxAllocatorState::~TTxAllocatorState()
{
    Alloc.Acquire();
}

std::pair<NKikimr::NMiniKQL::TType*, NUdf::TUnboxedValue> TTxAllocatorState::GetInternalBindingValue(
    const NKqpProto::TKqpPhyParamBinding& paramBinding)
{
    auto& internalBinding = paramBinding.GetInternalBinding();
    switch (internalBinding.GetType()) {
        case NKqpProto::TKqpPhyInternalBinding::PARAM_NOW:
            return {TypeEnv.GetUi64(), TUnboxedValuePod(ui64(GetCachedNow()))};
        case NKqpProto::TKqpPhyInternalBinding::PARAM_CURRENT_DATE: {
            ui32 date = GetCachedDate();
            YQL_ENSURE(date <= Max<ui32>());
            return {TypeEnv.GetUi32(), TUnboxedValuePod(ui32(date))};
        }
        case NKqpProto::TKqpPhyInternalBinding::PARAM_CURRENT_DATETIME: {
            ui64 datetime = GetCachedDatetime();
            YQL_ENSURE(datetime <= Max<ui32>());
            return {TypeEnv.GetUi32(), TUnboxedValuePod(ui32(datetime))};
        }
        case NKqpProto::TKqpPhyInternalBinding::PARAM_CURRENT_TIMESTAMP:
            return {TypeEnv.GetUi64(), TUnboxedValuePod(ui64(GetCachedTimestamp()))};
        case NKqpProto::TKqpPhyInternalBinding::PARAM_RANDOM_NUMBER:
            return {TypeEnv.GetUi64(), TUnboxedValuePod(ui64(GetCachedRandom<ui64>()))};
        case NKqpProto::TKqpPhyInternalBinding::PARAM_RANDOM:
            return {NKikimr::NMiniKQL::TDataType::Create(NUdf::TDataType<double>::Id, TypeEnv),
                TUnboxedValuePod(double(GetCachedRandom<double>()))};
        case NKqpProto::TKqpPhyInternalBinding::PARAM_RANDOM_UUID: {
            auto uuid = GetCachedRandom<TGUID>();
            const auto ptr = reinterpret_cast<ui8*>(uuid.dw);
            union {
                ui64 half[2];
                char bytes[16];
            } buf;
            buf.half[0] = *reinterpret_cast<ui64*>(ptr);
            buf.half[1] = *reinterpret_cast<ui64*>(ptr + 8);
            return {NKikimr::NMiniKQL::TDataType::Create(NUdf::TDataType<NUdf::TUuid>::Id, TypeEnv), MakeString(TStringRef(buf.bytes, 16))};
        }
        default:
            YQL_ENSURE(false, "Unexpected internal parameter type: " << (ui32)internalBinding.GetType());
    }
}

TQueryData::TQueryData(const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    TIntrusivePtr<ITimeProvider> timeProvider, TIntrusivePtr<IRandomProvider> randomProvider)
    : TQueryData(std::make_shared<TTxAllocatorState>(functionRegistry, timeProvider, randomProvider))
{
}

TQueryData::TQueryData(TTxAllocatorState::TPtr allocatorState)
    : AllocState(std::move(allocatorState))
{
}

TQueryData::~TQueryData() {
    {
        auto g = TypeEnv().BindAllocator();
        TTxResultVector emptyVector;
        TxResults.swap(emptyVector);
        TUnboxedParamsMap emptyMap;
        UnboxedData.swap(emptyMap);
    }
}

const TQueryData::TParamMap& TQueryData::GetParams() {
    for(auto& [name, _] : UnboxedData) {
        GetParameterMiniKqlValue(name);
    }

    return Params;
}

NKikimr::NMiniKQL::TType* TQueryData::GetParameterType(const TString& name) {
    auto it = UnboxedData.find(name);
    if (it == UnboxedData.end()) {
        return nullptr;
    }

    return it->second.first;
}

bool TQueryData::AddUVParam(const TString& name, NKikimr::NMiniKQL::TType* type, const NUdf::TUnboxedValue& value) {
    auto g = TypeEnv().BindAllocator();
    auto [_, success] = UnboxedData.emplace(name, std::make_pair(type, value));
    return success;
}

bool TQueryData::AddTypedValueParam(const TString& name, const Ydb::TypedValue& param) {
    auto guard = TypeEnv().BindAllocator();
    auto [typeFromProto, value] = ImportValueFromProto(
        param.type(), param.value(), TypeEnv(), AllocState->HolderFactory);
    return AddUVParam(name, typeFromProto, value);
}

bool TQueryData::AddMkqlParam(const TString& name, const NKikimrMiniKQL::TType& t, const NKikimrMiniKQL::TValue& v) {
    auto guard = TypeEnv().BindAllocator();
    auto [typeFromProto, value] = ImportValueFromProto(t, v, TypeEnv(), AllocState->HolderFactory);
    return AddUVParam(name, typeFromProto, value);
}

std::pair<NKikimr::NMiniKQL::TType*, NUdf::TUnboxedValue> TQueryData::GetInternalBindingValue(
    const NKqpProto::TKqpPhyParamBinding& paramBinding)
{
    return AllocState->GetInternalBindingValue(paramBinding);
}

TQueryData::TTypedUnboxedValue& TQueryData::GetParameterUnboxedValue(const TString& name) {
    auto it = UnboxedData.find(name);
    YQL_ENSURE(it != UnboxedData.end(), "Param " << name << " not found");
    return it->second;
}

const NKikimrMiniKQL::TParams* TQueryData::GetParameterMiniKqlValue(const TString& name) {
    if (UnboxedData.find(name) == UnboxedData.end())
        return nullptr;

    auto it = Params.find(name);
    if (it == Params.end()) {
        with_lock(AllocState->Alloc) {
            const auto& [type, uv] = GetParameterUnboxedValue(name);
            NKikimrMiniKQL::TParams param;
            ExportTypeToProto(type, *param.MutableType());
            ExportValueToProto(type, uv, *param.MutableValue());

            auto [nit, success] = Params.emplace(name, std::move(param));
            YQL_ENSURE(success);

            return &(nit->second);
        }
    }

    return &(it->second);
}

const NKikimr::NMiniKQL::TTypeEnvironment& TQueryData::TypeEnv() {
    return AllocState->TypeEnv;
}

bool TQueryData::MaterializeParamValue(bool ensure, const NKqpProto::TKqpPhyParamBinding& paramBinding) {
    switch (paramBinding.GetTypeCase()) {
        case NKqpProto::TKqpPhyParamBinding::kExternalBinding: {
            const auto* clientParam = GetParameterType(paramBinding.GetName());
            if (clientParam) {
                return true;
            }
            Y_ENSURE(!ensure || clientParam, "Parameter not found: " << paramBinding.GetName());
            return false;
        }
        case NKqpProto::TKqpPhyParamBinding::kTxResultBinding: {
            auto& txResultBinding = paramBinding.GetTxResultBinding();
            auto txIndex = txResultBinding.GetTxIndex();
            auto resultIndex = txResultBinding.GetResultIndex();

            if (HasResult(txIndex, resultIndex)) {
                auto guard = TypeEnv().BindAllocator();
                auto [type, value] = GetTxResult(txIndex, resultIndex);
                AddUVParam(paramBinding.GetName(), type, value);
                return true;
            }

            if (ensure) {
                YQL_ENSURE(HasResult(txIndex, resultIndex));
            }
            return false;
        }
        case NKqpProto::TKqpPhyParamBinding::kInternalBinding: {
            auto guard = TypeEnv().BindAllocator();
            auto [type, value] = GetInternalBindingValue(paramBinding);
            AddUVParam(paramBinding.GetName(), type, value);
            return true;
        }
        default:
            YQL_ENSURE(false, "Unexpected parameter binding type: " << (ui32)paramBinding.GetTypeCase());
    }

    return false;
}

NDqProto::TData TQueryData::SerializeParamValue(const TString& name) {
    const auto& [type, value] = GetParameterUnboxedValue(name);
    return NDq::TDqDataSerializer::SerializeParamValue(type, value);
}

void TQueryData::Clear() {
    {
        auto g = TypeEnv().BindAllocator();
        Params.clear();
        TUnboxedParamsMap emptyMap;
        UnboxedData.swap(emptyMap);
        TTxResultVector emptyVector;
        TxResults.swap(emptyVector);
        AllocState->Reset();
    }
}

} // namespace NKikimr::NKqp