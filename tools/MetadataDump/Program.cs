using System.Collections.Immutable;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;

// Offline, read-only metadata dump of an IL2CPP interop assembly. Prints the declaration surface of
// the types whose names contain any of the given filters. Nothing is loaded or executed.
internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: metadatadump <assembly> <type-filter> [<type-filter>...]");
            return 2;
        }

        using var stream = File.OpenRead(args[0]);
        using var pe = new PEReader(stream);
        var reader = pe.GetMetadataReader();
        var filters = args.Skip(1).ToArray();

        if (filters.Length == 1 && filters[0].StartsWith("--declares=", StringComparison.Ordinal))
        {
            var wanted = filters[0]["--declares=".Length..];
            foreach (var handle in reader.TypeDefinitions)
            {
                var type = reader.GetTypeDefinition(handle);
                var full = FullName(reader, type);
                foreach (var propertyHandle in type.GetProperties())
                {
                    var propertyName = reader.GetString(reader.GetPropertyDefinition(propertyHandle).Name);
                    if (propertyName == wanted) Console.WriteLine($"PROP {full}.{propertyName}");
                }
                foreach (var fieldHandle in type.GetFields())
                {
                    var fieldName = reader.GetString(reader.GetFieldDefinition(fieldHandle).Name);
                    if (fieldName == wanted) Console.WriteLine($"FIELD {full}.{fieldName}");
                }
                foreach (var methodHandle in type.GetMethods())
                {
                    var methodName = reader.GetString(reader.GetMethodDefinition(methodHandle).Name);
                    if (methodName == wanted) Console.WriteLine($"METHOD {full}.{methodName}");
                }
            }
            return 0;
        }

        if (filters.Length == 1 && filters[0].StartsWith("--owner-of=", StringComparison.Ordinal))
        {
            var wanted = filters[0]["--owner-of=".Length..];
            var provider = new TypeNameProvider();
            foreach (var handle in reader.TypeDefinitions)
            {
                var type = reader.GetTypeDefinition(handle);
                var full = FullName(reader, type);
                foreach (var propertyHandle in type.GetProperties())
                {
                    var property = reader.GetPropertyDefinition(propertyHandle);
                    string declared;
                    try { declared = property.DecodeSignature(provider, null).ReturnType; }
                    catch { continue; }
                    if (!declared.Contains(wanted, StringComparison.Ordinal)) continue;
                    Console.WriteLine($"{full}.{reader.GetString(property.Name)} : {declared}");
                }
                foreach (var fieldHandle in type.GetFields())
                {
                    var field = reader.GetFieldDefinition(fieldHandle);
                    string declared;
                    try { declared = field.DecodeSignature(provider, null); }
                    catch { continue; }
                    if (!declared.Contains(wanted, StringComparison.Ordinal)) continue;
                    Console.WriteLine($"{full}.{reader.GetString(field.Name)} : {declared}");
                }
                foreach (var methodHandle in type.GetMethods())
                {
                    var method = reader.GetMethodDefinition(methodHandle);
                    MethodSignature<string> signature;
                    try { signature = method.DecodeSignature(provider, null); }
                    catch { continue; }
                    var hit = signature.ParameterTypes.Any(p => p.Contains(wanted, StringComparison.Ordinal)) ||
                              signature.ReturnType.Contains(wanted, StringComparison.Ordinal);
                    if (hit) Console.WriteLine($"{full}.{reader.GetString(method.Name)} : {signature.ReturnType} ({string.Join(", ", signature.ParameterTypes)})");
                }
            }
            return 0;
        }

        foreach (var handle in reader.TypeDefinitions)
        {
            var type = reader.GetTypeDefinition(handle);
            var full = FullName(reader, type);
            if (!filters.Any(f => full.Contains(f, StringComparison.Ordinal))) continue;

            Console.WriteLine($"TYPE {full}  [{(type.Attributes & TypeAttributes.ClassSemanticsMask) switch
            {
                TypeAttributes.ClassSemanticsMask => "interface",
                TypeAttributes.ClassSemanticsMask + 1 => "struct",
                _ => "class",
            }}] base={BaseName(reader, type)}");

            foreach (var fieldHandle in type.GetFields())
            {
                var field = reader.GetFieldDefinition(fieldHandle);
                var flags = field.Attributes;
                if ((flags & FieldAttributes.Static) != 0 &&
                    reader.GetString(field.Name).StartsWith("Native", StringComparison.Ordinal)) continue;
                string typeName;
                try
                {
                    typeName = field.DecodeSignature(new TypeNameProvider(), null);
                }
                catch (Exception exception)
                {
                    typeName = "<" + exception.GetType().Name + ">";
                }
                var access = (flags & FieldAttributes.FieldAccessMask) switch
                {
                    FieldAttributes.Public => "public",
                    FieldAttributes.Family => "protected",
                    FieldAttributes.Assembly => "internal",
                    _ => "private",
                };
                var mods = (flags & FieldAttributes.Static) != 0 ? "static " : "";
                var literal = (flags & FieldAttributes.Literal) != 0
                    ? " = " + reader.GetConstant(field.GetDefaultValue()).Value
                    : "";
                Console.WriteLine($"  FIELD {access} {mods}{typeName} {reader.GetString(field.Name)}{literal}");
            }

            foreach (var propertyHandle in type.GetProperties())
            {
                var property = reader.GetPropertyDefinition(propertyHandle);
                string typeName;
                try
                {
                    typeName = property.DecodeSignature(new TypeNameProvider(), null).ReturnType;
                }
                catch (Exception exception)
                {
                    typeName = "<" + exception.GetType().Name + ">";
                }
                var accessors = property.GetAccessors();
                var modes = (accessors.Getter.IsNil ? "" : "get") + (accessors.Setter.IsNil ? "" : "/set");
                Console.WriteLine($"  PROP {typeName} {reader.GetString(property.Name)} {modes}");
            }

            foreach (var methodHandle in type.GetMethods())
            {
                var method = reader.GetMethodDefinition(methodHandle);
                var flags = method.Attributes;
                if ((flags & MethodAttributes.MemberAccessMask) == MethodAttributes.Private &&
                    (flags & MethodAttributes.Static) == 0) continue;
                string signature;
                try
                {
                    var provider = new TypeNameProvider();
                    var blob = method.DecodeSignature(provider, null);
                    var parameters = method.GetParameters()
                        .Select(p => reader.GetParameter(p))
                        .Where(p => p.SequenceNumber > 0)
                        .OrderBy(p => p.SequenceNumber)
                        .Select(p => reader.GetString(p.Name))
                        .ToArray();
                    var parts = new List<string>();
                    for (var i = 0; i < blob.ParameterTypes.Length; i++)
                    {
                        var pname = i < parameters.Length ? parameters[i] : "p" + i;
                        parts.Add(blob.ParameterTypes[i] + " " + pname);
                    }
                    signature = $"{blob.ReturnType} ({string.Join(", ", parts)})";
                }
                catch (Exception exception)
                {
                    signature = "<" + exception.GetType().Name + ">";
                }
                var access = (flags & MethodAttributes.MemberAccessMask) switch
                {
                    MethodAttributes.Public => "public",
                    MethodAttributes.Family => "protected",
                    MethodAttributes.Assembly => "internal",
                    _ => "private",
                };
                var mods = (flags & MethodAttributes.Static) != 0 ? "static " : "";
                Console.WriteLine($"  METHOD {access} {mods}{signature} {reader.GetString(method.Name)}");
            }
            Console.WriteLine();
        }
        return 0;
    }

    private static string FullName(MetadataReader reader, TypeDefinition type)
    {
        var name = reader.GetString(type.Name);
        var declaring = type.GetDeclaringType();
        if (!declaring.IsNil) return FullName(reader, reader.GetTypeDefinition(declaring)) + "." + name;
        var ns = reader.GetString(type.Namespace);
        return ns.Length == 0 ? name : ns + "." + name;
    }

    private static string BaseName(MetadataReader reader, TypeDefinition type)
    {
        if (type.BaseType.IsNil) return "<none>";
        return type.BaseType.Kind switch
        {
            HandleKind.TypeReference => reader.GetString(reader.GetTypeReference((TypeReferenceHandle)type.BaseType).Name),
            HandleKind.TypeDefinition => reader.GetString(reader.GetTypeDefinition((TypeDefinitionHandle)type.BaseType).Name),
            _ => "<unknown>",
        };
    }

    private sealed class TypeNameProvider : ISignatureTypeProvider<string, object?>
    {
        public string GetArrayType(string elementType, ArrayShape shape) => elementType + "[" + new string(',', shape.Rank - 1) + "]";
        public string GetByReferenceType(string elementType) => "ref " + elementType;
        public string GetFunctionPointerType(MethodSignature<string> signature) => "fnptr";
        public string GetGenericInstantiation(string genericType, ImmutableArray<string> typeArguments) =>
            genericType + "<" + string.Join(", ", typeArguments) + ">";
        public string GetGenericMethodParameter(object? genericContext, int index) => "!!" + index;
        public string GetGenericTypeParameter(object? genericContext, int index) => "!" + index;
        public string GetModifiedType(string modifier, string unmodifiedType, bool isRequired) => unmodifiedType;
        public string GetPinnedType(string elementType) => elementType;
        public string GetPointerType(string elementType) => elementType + "*";
        public string GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode.ToString();
        public string GetSZArrayType(string elementType) => elementType + "[]";
        public string GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) =>
            reader.GetString(reader.GetTypeDefinition(handle).Name);
        public string GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind) =>
            reader.GetString(reader.GetTypeReference(handle).Name);
        public string GetTypeFromSpecification(MetadataReader reader, object? genericContext, TypeSpecificationHandle handle, byte rawTypeKind) =>
            "spec";
    }
}
