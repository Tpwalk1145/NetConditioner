using System.Text.Json.Serialization;

namespace NetConditioner.Models;

/// <summary>
/// config.json 的 System.Text.Json 源生成上下文。
/// NativeAOT/裁剪环境下不能使用基于反射的序列化，必须走源生成。
/// 输出格式与旧版保持一致：camelCase 属性名、缩进、枚举写成字符串。
/// </summary>
[JsonSourceGenerationOptions(
    WriteIndented = true,
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    UseStringEnumConverter = true)]
[JsonSerializable(typeof(AppConfig))]
internal sealed partial class ConfigJsonContext : JsonSerializerContext
{
}
