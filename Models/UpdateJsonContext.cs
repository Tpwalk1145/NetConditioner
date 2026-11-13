using System.Text.Json.Serialization;

namespace NetConditioner.Models;

/// <summary>
/// latest.json 的 System.Text.Json 源生成上下文。
/// NativeAOT/裁剪环境下不能使用基于反射的序列化。
/// </summary>
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(UpdateManifest))]
internal sealed partial class UpdateJsonContext : JsonSerializerContext
{
}
