/* Native shader MCP implementation. No extension runtime or capture-file writes. */
#include "MCPShaderTools.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <cstring>
#include "Code/Interface/PersistentConfig.h"
#include "Code/Interface/QRDInterface.h"
#include "MCPTools.h"

namespace MCP
{
namespace
{
QString Str(const rdcstr &s)
{
  return QString::fromUtf8(s.c_str());
}
ToolResult Fail(ErrorCode code, const char *message)
{
  return ToolResult::Failure(Error(code, message));
}
ToolResult Interrupted(CallContext &call)
{
  if(call.IsCancelled())
    return Fail(ErrorCode::Cancelled, "Request cancelled");
  if(call.IsExpired())
    return Fail(ErrorCode::Timeout, "Request deadline passed");
  return ToolResult::Success(QJsonObject());
}
bool ReadId(const QJsonValue &value, ResourceId &id)
{
  uint64_t n = 0;
  if(value.isString())
  {
    QString s = value.toString();
    if(s.startsWith(QStringLiteral("ResourceId::")))
      s = s.mid(12);
    bool ok = false;
    n = s.toULongLong(&ok, s.startsWith(QStringLiteral("0x")) ? 16 : 10);
    if(!ok)
      return false;
  }
  else if(value.isDouble())
  {
    double d = value.toDouble();
    if(!std::isfinite(d) || d < 0 || d > 9007199254740991.0 || std::floor(d) != d)
      return false;
    n = uint64_t(d);
  }
  else
    return false;
  memcpy(&id, &n, sizeof(n));
  return n != 0;
}
QString Id(ResourceId id)
{
  uint64_t n = 0;
  memcpy(&n, &id, sizeof(n));
  return QString::number(qulonglong(n));
}
QString StageName(ShaderStage stage)
{
  return detail::ShaderStageName(stage).toLower();
}
QJsonArray Descriptors(const rdcarray<UsedDescriptor> &descriptors)
{
  QJsonArray out;
  for(const UsedDescriptor &d : descriptors)
    out.push_back(QJsonObject{
        {QStringLiteral("resourceId"), Id(d.descriptor.resource)},
        {QStringLiteral("index"), int(d.access.index)},
        {QStringLiteral("arrayElement"), double(d.access.arrayElement)},
        {QStringLiteral("byteOffset"), QString::number(qulonglong(d.descriptor.byteOffset))},
        {QStringLiteral("byteSize"), QString::number(qulonglong(d.descriptor.byteSize))},
        {QStringLiteral("staticallyUnused"), d.access.staticallyUnused}});
  return out;
}
QJsonArray Signature(const rdcarray<SigParameter> &signature)
{
  QJsonArray out;
  for(const SigParameter &s : signature)
    out.push_back(QJsonObject{{QStringLiteral("varName"), Str(s.varName)},
                              {QStringLiteral("semanticName"), Str(s.semanticName)},
                              {QStringLiteral("semanticIndex"), double(s.semanticIndex)},
                              {QStringLiteral("regIndex"), double(s.regIndex)},
                              {QStringLiteral("compCount"), double(s.compCount)},
                              {QStringLiteral("varType"), detail::VarTypeName(s.varType)}});
  return out;
}
QJsonArray Bindings(const rdcarray<ShaderResource> &resources)
{
  QJsonArray out;
  for(const ShaderResource &r : resources)
    out.push_back(QJsonObject{{QStringLiteral("name"), Str(r.name)},
                              {QStringLiteral("bindPoint"), double(r.fixedBindNumber)},
                              {QStringLiteral("bindSet"), double(r.fixedBindSetOrSpace)},
                              {QStringLiteral("bindArraySize"), double(r.bindArraySize)},
                              {QStringLiteral("textureType"), detail::TextureTypeName(r.textureType)},
                              {QStringLiteral("descriptorType"), int(r.descriptorType)}});
  return out;
}
bool HasEvent(const rdcarray<ActionDescription> &actions, uint32_t event)
{
  for(const ActionDescription &a : actions)
    if(a.eventId == event || HasEvent(a.children, event))
      return true;
  return false;
}
QJsonObject Type(const char *type)
{
  return QJsonObject{{QStringLiteral("type"), QString::fromUtf8(type)}};
}
QJsonObject Integer(double min, double max)
{
  QJsonObject o = Type("integer");
  o[QStringLiteral("minimum")] = min;
  o[QStringLiteral("maximum")] = max;
  return o;
}
QJsonObject Schema(const QJsonObject &properties, const QJsonArray &required = QJsonArray())
{
  return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"), properties},
                     {QStringLiteral("required"), required},
                     {QStringLiteral("additionalProperties"), false}};
}
}

bool ParseShaderStage(const QString &name, ShaderStage &stage)
{
  const QString n = name.toLower();
  const QStringList aliases[] = {
      {QStringLiteral("vertex"), QStringLiteral("vs")},
      {QStringLiteral("hull"), QStringLiteral("tesscontrol"), QStringLiteral("tesc"),
       QStringLiteral("hs")},
      {QStringLiteral("domain"), QStringLiteral("tesseval"), QStringLiteral("tese"),
       QStringLiteral("ds")},
      {QStringLiteral("geometry"), QStringLiteral("gs")},
      {QStringLiteral("pixel"), QStringLiteral("fragment"), QStringLiteral("ps"),
       QStringLiteral("fs")},
      {QStringLiteral("compute"), QStringLiteral("cs")},
      {QStringLiteral("task"), QStringLiteral("amplification"), QStringLiteral("as")},
      {QStringLiteral("mesh"), QStringLiteral("ms")},
      {QStringLiteral("raygen")},
      {QStringLiteral("intersection")},
      {QStringLiteral("anyhit")},
      {QStringLiteral("closesthit")},
      {QStringLiteral("miss")},
      {QStringLiteral("callable")}};
  for(int i = 0; i < int(sizeof(aliases) / sizeof(aliases[0])); i++)
    if(aliases[i].contains(n))
    {
      stage = ShaderStage(i);
      return true;
    }
  return false;
}

ToolResult ParseShaderEditRequest(const QJsonObject &args, ShaderEditRequest &request)
{
  ShaderEditRequest parsed;
  if(!ReadId(args.value(QStringLiteral("resourceId")), parsed.resource))
    return Fail(ErrorCode::InvalidParams,
                "resourceId must be a nonzero opaque decimal id (use a string for 64-bit ids)");
  auto enumValue = [&](const char *key, int count, int &out) {
    QJsonValue v = args.value(QString::fromUtf8(key));
    double d = v.toDouble(-1);
    if(!v.isDouble() || !std::isfinite(d) || std::floor(d) != d || d < 0 || d >= count)
      return false;
    out = int(d);
    return true;
  };
  int stage = 0, encoding = 0;
  if(!enumValue("shaderStage", int(ShaderStage::Count), stage) ||
     !enumValue("sourceEncoding", int(ShaderEncoding::Count), encoding))
    return Fail(ErrorCode::InvalidParams, "Invalid shaderStage or sourceEncoding enum");
  parsed.stage = ShaderStage(stage);
  parsed.encoding = ShaderEncoding(encoding);
  if(!IsTextRepresentation(parsed.encoding))
    return Fail(ErrorCode::Unsupported,
                "files[].contents requires a text shader encoding; binary blobs are not accepted");
  parsed.entryPoint = args.value(QStringLiteral("entryPoint")).toString();
  if(parsed.entryPoint.isEmpty() || parsed.entryPoint.contains(QChar(0)))
    return Fail(ErrorCode::InvalidParams, "entryPoint must be a nonempty string without NUL");
  const QJsonValue fv = args.value(QStringLiteral("files"));
  if(!fv.isArray() || fv.toArray().isEmpty())
    return Fail(ErrorCode::InvalidParams, "files must be a nonempty array");
  QSet<QString> names;
  for(const QJsonValue &v : fv.toArray())
  {
    const QJsonObject file = v.toObject();
    const QString filename = file.value(QStringLiteral("filename")).toString();
    const QString contents = file.value(QStringLiteral("contents")).toString();
    QString normalized =
        QDir::cleanPath(QString(filename).replace(QLatin1Char('\\'), QLatin1Char('/')));
    if(!v.isObject() || filename.isEmpty() || filename.contains(QChar(0)) ||
       !file.value(QStringLiteral("contents")).isString() || contents.contains(QChar(0)) ||
       names.contains(normalized))
      return Fail(ErrorCode::InvalidParams,
                  "Shader files require unique filenames and string contents without NUL");
    names.insert(normalized);
    parsed.files.push_back(
        rdcpair<rdcstr, rdcstr>(filename.toUtf8().constData(), contents.toUtf8().constData()));
  }
  if(args.contains(QStringLiteral("entryFileIndex")))
  {
    double d = args.value(QStringLiteral("entryFileIndex")).toDouble(-1);
    if(!args.value(QStringLiteral("entryFileIndex")).isDouble() || std::floor(d) != d || d < 0 ||
       d >= parsed.files.size())
      return Fail(ErrorCode::InvalidParams, "entryFileIndex is outside files");
    parsed.entryFileIndex = int(d);
  }
  if(args.contains(QStringLiteral("compileFlags")) &&
     !args.value(QStringLiteral("compileFlags")).isArray())
    return Fail(ErrorCode::InvalidParams, "compileFlags must be an array");
  for(const QJsonValue &v : args.value(QStringLiteral("compileFlags")).toArray())
  {
    QJsonObject flag = v.toObject();
    if(!v.isObject() || !flag.value(QStringLiteral("name")).isString() ||
       !flag.value(QStringLiteral("value")).isString())
      return Fail(ErrorCode::InvalidParams, "compileFlags entries require name and value strings");
    ShaderCompileFlag f;
    f.name = flag.value(QStringLiteral("name")).toString().toUtf8().constData();
    f.value = flag.value(QStringLiteral("value")).toString().toUtf8().constData();
    parsed.flags.flags.push_back(f);
  }
  request = parsed;
  return ToolResult::Success(QJsonObject());
}

ToolResult PrepareShaderEditSource(const ShaderEditRequest &request, QByteArray &source,
                                   CallContext &call)
{
  // Expand only supplied virtual files, respecting pragma-once and cycle guards.
  // Conditional includes cannot be evaluated without the driver's preprocessor:
  // reject those rather than silently compiling a different program.
  QSet<int> stack, once;
  std::function<ToolResult(int, QString &)> expand = [&](int index, QString &out) -> ToolResult {
    ToolResult stop = Interrupted(call);
    if(!stop.ok)
      return stop;
    if(once.contains(index))
      return ToolResult::Success(QJsonObject());
    if(stack.contains(index))
      return Fail(ErrorCode::InvalidParams, "Cyclic shader include");
    stack.insert(index);
    QString text = Str(request.files[index].second);
    const QStringList lines = text.split(QLatin1Char('\n'));
    int conditional = 0;
    bool blockComment = false;
    static const QRegularExpression include(
        QStringLiteral("^\\s*#\\s*include\\s*[\"<]([^\">]+)[\">]\\s*(?://.*)?$"));
    static const QRegularExpression includeStart(QStringLiteral("^\\s*#\\s*include\\b"));
    static const QRegularExpression ifStart(QStringLiteral("^\\s*#\\s*(if|ifdef|ifndef)\\b"));
    static const QRegularExpression endif(QStringLiteral("^\\s*#\\s*endif\\b"));
    static const QRegularExpression pragma(QStringLiteral("^\\s*#\\s*pragma\\s+once\\b"));
    for(const QString &line : lines)
    {
      stop = Interrupted(call);
      if(!stop.ok)
        return stop;
      // Recognise directives on comment-stripped text, while preserving source bytes.
      QString directive;
      QChar quote;
      for(int c = 0; c < line.size(); c++)
      {
        const QChar ch = line[c];
        const QChar next = c + 1 < line.size() ? line[c + 1] : QChar();
        if(blockComment)
        {
          if(ch == QLatin1Char('*') && next == QLatin1Char('/'))
          {
            blockComment = false;
            c++;
          }
          continue;
        }
        if(!quote.isNull())
        {
          directive += ch;
          if(ch == QLatin1Char('\\') && c + 1 < line.size())
            directive += line[++c];
          else if(ch == quote)
            quote = QChar();
          continue;
        }
        if(ch == QLatin1Char('/') && next == QLatin1Char('/'))
          break;
        if(ch == QLatin1Char('/') && next == QLatin1Char('*'))
        {
          blockComment = true;
          directive += QLatin1Char(' ');
          c++;
          continue;
        }
        if(ch == QLatin1Char('"') || ch == QLatin1Char('\''))
          quote = ch;
        directive += ch;
      }
      if(directive.trimmed().endsWith(QLatin1Char('\\')))
        return Fail(ErrorCode::Unsupported, "Line continuations require preprocessed source");
      if(ifStart.match(directive).hasMatch())
        conditional++;
      if(endif.match(directive).hasMatch())
        conditional--;
      if(pragma.match(directive).hasMatch())
      {
        if(conditional != 0)
          return Fail(ErrorCode::Unsupported,
                      "Conditional pragma once requires preprocessed source");
        once.insert(index);
        out += QLatin1Char('\n');
        continue;
      }
      auto match = include.match(directive);
      if(includeStart.match(directive).hasMatch())
      {
        if(!match.hasMatch() || conditional != 0)
          return Fail(ErrorCode::Unsupported,
                      "Macro or conditional includes are unsupported; supply preprocessed source");
        QString needle = QDir::cleanPath(match.captured(1));
        QString parent = QFileInfo(Str(request.files[index].first)).path();
        int found = -1;
        for(int i = 0; i < request.files.count(); i++)
        {
          QString name = QDir::cleanPath(
              QString(Str(request.files[i].first)).replace(QLatin1Char('\\'), QLatin1Char('/')));
          if(name == needle || name == QDir::cleanPath(parent + QLatin1Char('/') + needle))
          {
            if(found >= 0 && found != i)
              return Fail(ErrorCode::InvalidParams, "Ambiguous shader include");
            found = i;
          }
        }
        if(found < 0)
          return Fail(ErrorCode::InvalidParams,
                      "Include is absent from supplied files; host files are never read");
        QString child;
        ToolResult r = expand(found, child);
        if(!r.ok)
          return r;
        out += child + QLatin1Char('\n');
      }
      else
        out += line + QLatin1Char('\n');
    }
    stack.remove(index);
    return ToolResult::Success(QJsonObject());
  };
  QString combined;
  ToolResult result = expand(request.entryFileIndex, combined);
  if(result.ok)
    source = combined.toUtf8();
  return result;
}

QJsonObject ShaderVariableToJson(const ShaderVariable &v)
{
  QJsonObject out{{QStringLiteral("name"), Str(v.name)},
                  {QStringLiteral("type"), detail::VarTypeName(v.type)},
                  {QStringLiteral("rows"), int(v.rows)},
                  {QStringLiteral("columns"), int(v.columns)}};
  QJsonArray members;
  for(const ShaderVariable &m : v.members)
    members.push_back(ShaderVariableToJson(m));
  out[QStringLiteral("members")] = members;
  QJsonArray values;
  for(int i = 0; i < qMin(16, int(v.rows) * int(v.columns)) && v.members.empty(); i++)
  {
    switch(v.type)
    {
      case VarType::Float: values.push_back(double(v.value.f32v[i])); break;
      case VarType::Half: values.push_back(double(float(v.value.f16v[i]))); break;
      case VarType::Double: values.push_back(v.value.f64v[i]); break;
      case VarType::SInt: values.push_back(v.value.s32v[i]); break;
      case VarType::SShort: values.push_back(int(v.value.s16v[i])); break;
      case VarType::SByte: values.push_back(int(v.value.s8v[i])); break;
      case VarType::UShort: values.push_back(int(v.value.u16v[i])); break;
      case VarType::UByte: values.push_back(int(v.value.u8v[i])); break;
      case VarType::UInt:
      case VarType::Bool:
      case VarType::Enum: values.push_back(double(v.value.u32v[i])); break;
      case VarType::SLong: values.push_back(QString::number(qlonglong(v.value.s64v[i]))); break;
      case VarType::ULong:
      case VarType::GPUPointer:
        values.push_back(QString::number(qulonglong(v.value.u64v[i])));
        break;
      default: break;
    }
  }
  out[QStringLiteral("value")] = values;
  return out;
}

QJsonObject ShaderReflectionToJson(const ShaderReflection &r, bool includeSource)
{
  QJsonObject out{{QStringLiteral("resourceId"), Id(r.resourceId)},
                  {QStringLiteral("entryPoint"), Str(r.entryPoint)},
                  {QStringLiteral("stage"), detail::ShaderStageName(r.stage)},
                  {QStringLiteral("shaderStage"), int(r.stage)},
                  {QStringLiteral("encoding"), int(r.encoding)},
                  {QStringLiteral("rawByteLength"), double(r.rawBytes.size())},
                  {QStringLiteral("outputTopology"), detail::TopologyName(r.outputTopology)},
                  {QStringLiteral("inputSignature"), Signature(r.inputSignature)},
                  {QStringLiteral("outputSignature"), Signature(r.outputSignature)},
                  {QStringLiteral("readOnlyResources"), Bindings(r.readOnlyResources)},
                  {QStringLiteral("readWriteResources"), Bindings(r.readWriteResources)}};
  QJsonArray threads;
  for(int i = 0; i < 3; i++)
    threads.push_back(double(r.dispatchThreadsDimension[i]));
  out[QStringLiteral("dispatchThreadsDimension")] = threads;
  QJsonArray blocks;
  for(const ConstantBlock &cb : r.constantBlocks)
    blocks.push_back(QJsonObject{{QStringLiteral("name"), Str(cb.name)},
                                 {QStringLiteral("byteSize"), double(cb.byteSize)},
                                 {QStringLiteral("bindPoint"), double(cb.fixedBindNumber)},
                                 {QStringLiteral("bindSet"), double(cb.fixedBindSetOrSpace)},
                                 {QStringLiteral("bindArraySize"), double(cb.bindArraySize)},
                                 {QStringLiteral("bufferBacked"), cb.bufferBacked},
                                 {QStringLiteral("variableCount"), double(cb.variables.size())}});
  out[QStringLiteral("constantBlocks")] = blocks;
  QJsonArray samplers;
  for(const ShaderSampler &s : r.samplers)
    samplers.push_back(QJsonObject{{QStringLiteral("name"), Str(s.name)},
                                   {QStringLiteral("bindPoint"), double(s.fixedBindNumber)},
                                   {QStringLiteral("bindSet"), double(s.fixedBindSetOrSpace)},
                                   {QStringLiteral("bindArraySize"), double(s.bindArraySize)}});
  out[QStringLiteral("samplers")] = samplers;
  const ShaderDebugInfo &d = r.debugInfo;
  out[QStringLiteral("compiler")] = int(d.compiler);
  out[QStringLiteral("sourceEncoding")] = int(d.encoding);
  out[QStringLiteral("entrySourceName")] = Str(d.entrySourceName);
  out[QStringLiteral("hasSource")] = !d.files.empty();
  out[QStringLiteral("editable")] = !d.files.empty() && IsTextRepresentation(d.encoding);
  int entry = d.editBaseFile;
  if(entry < 0 || entry >= d.files.count())
    entry = int(d.entryLocation.fileIndex);
  if(entry < 0 || entry >= d.files.count())
    entry = d.files.empty() ? -1 : 0;
  out[QStringLiteral("entryFileIndex")] = entry;
  QJsonArray flags;
  for(const ShaderCompileFlag &f : d.compileFlags.flags)
    flags.push_back(QJsonObject{{QStringLiteral("name"), Str(f.name)},
                                {QStringLiteral("value"), Str(f.value)}});
  out[QStringLiteral("compileFlags")] = flags;
  QJsonArray files;
  for(const ShaderSourceFile &f : d.files)
  {
    QJsonObject file{{QStringLiteral("filename"), Str(f.filename)}};
    if(includeSource)
      file[QStringLiteral("contents")] = Str(f.contents);
    files.push_back(file);
  }
  out[QStringLiteral("sourceFiles")] = files;
  if(includeSource)
  {
    if(entry >= 0)
      out[QStringLiteral("source")] = Str(d.files[entry].contents);
    out[QStringLiteral("debugStatus")] = Str(d.debugStatus);
    out[QStringLiteral("debugInfoLoadingLog")] = Str(d.debugInfoLoadingLog);
  }
  return out;
}

ToolResult QueryAdapter::ShaderTool(const QString &name, const QJsonObject &args, CallContext &call)
{
  ToolResult stop = Interrupted(call);
  if(!stop.ok)
    return stop;
  const bool apply = name == QStringLiteral("renderdoc_applyShaderEdit");
  const bool validate = name == QStringLiteral("renderdoc_validateShaderEdit");
  const bool sourceTool = name == QStringLiteral("renderdoc_getShaderSource");
  const bool info = name == QStringLiteral("renderdoc_getShaderInfo");
  const bool compare = name == QStringLiteral("renderdoc_compareShaders");
  const bool diagnostics = name == QStringLiteral("renderdoc_getShaderCompileDiagnostics");
  const bool variants = name == QStringLiteral("renderdoc_findShaderVariants");
  const bool project = name == QStringLiteral("renderdoc_findProjectImplementation");
  if(!apply && !validate && !sourceTool && !info && !compare && !diagnostics && !variants && !project)
    return Fail(ErrorCode::MethodNotFound, "Unknown shader tool");
  ShaderEditRequest edit;
  QByteArray bytes;
  if(apply || validate)
  {
    ToolResult parsed = ParseShaderEditRequest(args, edit);
    if(!parsed.ok)
      return parsed;
    parsed = PrepareShaderEditSource(edit, bytes, call);
    if(!parsed.ok)
      return parsed;
  }
  ShaderStage requested = ShaderStage::Count;
  QString stageArg = args.value(QStringLiteral("stage")).toString();
  if(!stageArg.isEmpty() && !ParseShaderStage(stageArg, requested))
    return Fail(ErrorCode::InvalidParams, "Unknown shader stage");
  if(requested >= ShaderStage::RayGen && requested != ShaderStage::Count &&
     (args.contains(QStringLiteral("eventId")) || compare))
    return Fail(ErrorCode::Unsupported,
                "Ray tracing event bindings are not exposed by the common PipeState stage "
                "interface; use resourceId reflection");
  const bool includeSource =
      sourceTool || compare ||
      args.value(QStringLiteral("includeSource")).toBool(info && !stageArg.isEmpty());
  if(info && !args.contains(QStringLiteral("eventId")) && !args.contains(QStringLiteral("resourceId")))
    return Fail(ErrorCode::InvalidParams, "eventId or resourceId is required");
  const bool includeCB = info && args.value(QStringLiteral("includeConstantBuffers"))
                                     .toBool(args.contains(QStringLiteral("eventId")));
  if(info && includeCB && !args.contains(QStringLiteral("eventId")))
    return Fail(ErrorCode::InvalidParams,
                "Decoded constant buffers require eventId; resource-only reflection retains "
                "constantBlocks metadata");
  QStringList roots, terms;
  QJsonArray sourceReferences;
  auto scanProject = [&]() -> ToolResult {
    terms.removeDuplicates();
    terms.removeAll(QString());
    if(terms.isEmpty())
      return Fail(ErrorCode::InvalidParams,
                  "Supply shaderName, passName, additionalTerms or a shader-bound event");
    const int limit = args.value(QStringLiteral("limit")).toInt(12);
    QJsonArray shaderHits, implementationHits, failures;
    int examined = 0, matched = 0;
    const QSet<QString> shaderExtensions = {
        QStringLiteral("shader"), QStringLiteral("hlsl"),  QStringLiteral("hlsli"),
        QStringLiteral("glsl"),   QStringLiteral("vert"),  QStringLiteral("frag"),
        QStringLiteral("comp"),   QStringLiteral("metal"), QStringLiteral("compute"),
        QStringLiteral("cginc"),  QStringLiteral("slang")};
    const QSet<QString> implementationExtensions = {QStringLiteral("cs"), QStringLiteral("cpp"),
                                                    QStringLiteral("c"), QStringLiteral("h"),
                                                    QStringLiteral("hpp")};
    QSet<QString> visited;
    for(const QString &root : roots)
    {
      QFileInfo ri(root);
      if(!ri.isAbsolute() || !ri.isDir())
      {
        failures.push_back(
            QJsonObject{{QStringLiteral("path"), root},
                        {QStringLiteral("reason"),
                         QStringLiteral("Root must be an existing absolute directory")}});
        continue;
      }
      QDirIterator it(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
      while(it.hasNext())
      {
        stop = Interrupted(call);
        if(!stop.ok)
          return stop;
        const QString path = it.next();
        QFileInfo fi(path);
        if(visited.contains(fi.canonicalFilePath()))
          continue;
        visited.insert(fi.canonicalFilePath());
        const QString ext = fi.suffix().toLower();
        const bool shader = shaderExtensions.contains(ext);
        if(!shader && !implementationExtensions.contains(ext))
          continue;
        examined++;
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly))
        {
          failures.push_back(QJsonObject{{QStringLiteral("path"), path},
                                         {QStringLiteral("reason"), file.errorString()}});
          continue;
        }
        int lineNumber = 0;
        bool fileMatched = false;
        while(!file.atEnd())
        {
          stop = Interrupted(call);
          if(!stop.ok)
            return stop;
          QString line = QString::fromUtf8(file.readLine());
          lineNumber++;
          QStringList found;
          for(const QString &term : terms)
            if(line.contains(term, Qt::CaseInsensitive) ||
               fi.fileName().contains(term, Qt::CaseInsensitive))
              found.push_back(term);
          if(found.isEmpty())
            continue;
          fileMatched = true;
          QJsonArray &hits = shader ? shaderHits : implementationHits;
          if(hits.size() < limit)
            hits.push_back(QJsonObject{{QStringLiteral("path"), path},
                                       {QStringLiteral("line"), lineNumber},
                                       {QStringLiteral("text"), line.trimmed()},
                                       {QStringLiteral("terms"), QJsonArray::fromStringList(found)},
                                       {QStringLiteral("confidence"), QStringLiteral("inferred")}});
          break;
        }
        if(fileMatched)
          matched++;
      }
    }
    return ToolResult::Success(QJsonObject{
        {QStringLiteral("available"), true},
        {QStringLiteral("projectRoots"), QJsonArray::fromStringList(roots)},
        {QStringLiteral("terms"), QJsonArray::fromStringList(terms)},
        {QStringLiteral("sourceReferences"), sourceReferences},
        {QStringLiteral("shaderMatches"), shaderHits},
        {QStringLiteral("implementationMatches"), implementationHits},
        {QStringLiteral("examinedFiles"), examined},
        {QStringLiteral("matchedFiles"), matched},
        {QStringLiteral("failures"), failures},
        {QStringLiteral("truncated"), matched > shaderHits.size() + implementationHits.size()},
        {QStringLiteral("limitation"),
         QStringLiteral("Text/name matches are candidate implementation evidence, not proof of "
                        "runtime use; symlinks are not followed.")}});
  };
  ToolResult result = WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
    if((apply || validate || includeSource || includeCB || project || diagnostics) &&
       !ctx.Config().MCP_AllowSensitiveData)
      return ToolResult::Failure(Error(
          ErrorCode::Unsupported, QStringLiteral("Sensitive shader/source/constant data is disabled"),
          QJsonObject{{QStringLiteral("setting"), QStringLiteral("MCP_AllowSensitiveData")},
                      {QStringLiteral("tool"), name}}));
    if(project)
    {
      for(const QJsonValue &v : args.value(QStringLiteral("projectRoots")).toArray())
        roots.push_back(v.toString());
      if(roots.isEmpty())
        return Fail(ErrorCode::InvalidParams,
                    "Native project search requires explicit projectRoots; there is no VS Code "
                    "workspace dependency");
      for(const char *key : {"shaderName", "passName"})
      {
        QString term = args.value(QString::fromUtf8(key)).toString();
        if(!term.isEmpty())
          terms.push_back(term);
      }
      for(const QJsonValue &v : args.value(QStringLiteral("additionalTerms")).toArray())
        if(!v.toString().isEmpty())
          terms.push_back(v.toString());
      uint32_t event = uint32_t(args.value(QStringLiteral("eventId")).toDouble(ctx.CurEvent()));
      if(ctx.IsCaptureLoaded() && event)
      {
        ToolResult facts =
            DispatchBlock("renderdoc_findProjectImplementation", call, [&](IReplayController &r) {
              if(!HasEvent(r.GetRootActions(), event))
                return Fail(ErrorCode::InvalidParams, "eventId does not exist");
              r.SetFrameEvent(event, false);
              const PipeState &pipe = r.GetPipelineState();
              for(int i = 0; i <= int(ShaderStage::Mesh); i++)
              {
                const ShaderReflection *reflection = pipe.GetShaderReflection(ShaderStage(i));
                if(!reflection)
                  continue;
                terms.push_back(Str(reflection->entryPoint));
                for(const ShaderSourceFile &f : reflection->debugInfo.files)
                {
                  sourceReferences.push_back(Str(f.filename));
                  terms.push_back(QFileInfo(Str(f.filename)).completeBaseName());
                }
              }
              return ToolResult::Success(QJsonObject());
            });
        if(!facts.ok)
          return facts;
        const ActionDescription *a = ctx.GetAction(event);
        if(a)
          terms.push_back(Str(a->customName));
      }
      return ToolResult::Success(QJsonObject());
    }
    if(!ctx.IsCaptureLoaded())
      return Fail(ErrorCode::NoCaptureLoaded, "No capture is loaded");
    if(apply || validate)
    {
      const ResourceDescription *resource = ctx.GetResource(edit.resource);
      if(!resource || resource->type != ResourceType::Shader)
        return Fail(ErrorCode::InvalidParams, "resourceId is not a captured shader");
      ResourceId compiled;
      bool published = false;
      ToolResult result =
          DispatchBlock(name.toUtf8().constData(), call, [&](IReplayController &r) -> ToolResult {
            bool stageMatches = false;
            for(const ShaderEntryPoint &entry : r.GetShaderEntryPoints(edit.resource))
              if(entry.stage == edit.stage)
                stageMatches = true;
            if(!stageMatches)
              return Fail(ErrorCode::InvalidParams,
                          "shaderStage is not an entry-point stage of the captured shader");
            if(!r.GetTargetShaderEncodings().contains(edit.encoding))
              return Fail(ErrorCode::Unsupported,
                          "Replay driver cannot build the requested sourceEncoding");
            auto built = r.BuildTargetShader(edit.entryPoint.toUtf8().constData(), edit.encoding,
                                             bytebuf(bytes), edit.flags, edit.stage);
            compiled = built.first;
            struct TargetGuard
            {
              IReplayController &replay;
              ResourceId &target;
              bool transferred = false;
              ~TargetGuard()
              {
                if(!transferred && target != ResourceId())
                {
                  replay.FreeTargetResource(target);
                  target = ResourceId();
                }
              }
            } guard{r, compiled};
            QJsonObject out{{QStringLiteral("available"), true},
                            {QStringLiteral("resourceId"), Id(edit.resource)},
                            {QStringLiteral("originalResourceId"), Id(edit.resource)},
                            {QStringLiteral("compiled"), compiled != ResourceId()},
                            {QStringLiteral("valid"), compiled != ResourceId()},
                            {QStringLiteral("applied"), false},
                            {QStringLiteral("compilerOutput"), Str(built.second)},
                            {QStringLiteral("diagnosticLocationBasis"),
                             QStringLiteral("Compiler-native output over expanded virtual source; "
                                            "locations are not remapped")}};
            ToolResult interrupted = Interrupted(call);
            if(!interrupted.ok)
            {
              if(compiled != ResourceId())
                r.FreeTargetResource(compiled);
              compiled = ResourceId();
              return interrupted;
            }
            if(!apply && compiled != ResourceId())
            {
              r.FreeTargetResource(compiled);
              compiled = ResourceId();
            }
            if(apply && compiled != ResourceId())
            {
              r.ReplaceResource(edit.resource, compiled);
              guard.transferred = true;
              published = true;
              out[QStringLiteral("applied")] = true;
              out[QStringLiteral("replacementResourceId")] = Id(compiled);
            }
            out[QStringLiteral("sideEffect")] =
                apply
                    ? QStringLiteral(
                          "Active replay replacement only; capture bytes are unchanged. "
                          "ReplayController owns the target resource until replay shutdown.")
                    : QStringLiteral("Compiled target resource released; no replacement applied.");
            return ToolResult::Success(out);
          });
      // UI transaction pins the capture throughout compile + mapping publication.
      if(published)
      {
        if(ctx.IsResourceReplaced(edit.resource))
          ctx.UnregisterReplacement(edit.resource);
        ctx.RegisterReplacement(edit.resource, compiled);
      }
      if(published && !result.ok)
      {
        result.error.data[QStringLiteral("sideEffectApplied")] = true;
        result.error.data[QStringLiteral("replacementResourceId")] = Id(compiled);
      }
      return result;
    }
    const uint32_t currentEvent = ctx.CurEvent();
    return DispatchBlock(name.toUtf8().constData(), call, [&](IReplayController &r) -> ToolResult {
      if(info && !args.contains(QStringLiteral("eventId")))
      {
        ResourceId shader, pipeline;
        if(!ReadId(args.value(QStringLiteral("resourceId")), shader))
          return Fail(ErrorCode::InvalidParams, "Invalid resourceId");
        if(args.contains(QStringLiteral("pipeline")) &&
           !ReadId(args.value(QStringLiteral("pipeline")), pipeline))
          return Fail(ErrorCode::InvalidParams, "Invalid pipeline");
        const rdcarray<ShaderEntryPoint> entries = r.GetShaderEntryPoints(shader);
        const QString entryName = args.value(QStringLiteral("entryPoint")).toString();
        const ShaderEntryPoint *selected = NULL;
        for(const ShaderEntryPoint &entry : entries)
          if((entryName.isEmpty() || Str(entry.name) == entryName) &&
             (requested == ShaderStage::Count || requested == entry.stage))
          {
            selected = &entry;
            break;
          }
        if(!selected)
          return Fail(ErrorCode::InvalidParams, "No matching shader entry point");
        const ShaderReflection *reflection = r.GetShader(pipeline, shader, *selected);
        if(!reflection)
          return Fail(ErrorCode::Unsupported, "Shader reflection unavailable");
        QJsonObject out = ShaderReflectionToJson(*reflection, includeSource);
        out[QStringLiteral("resourceId")] = ResourceRef(shader);
        if(includeSource)
          out[QStringLiteral("disassembly")] =
              Str(r.DisassembleShader(pipeline, reflection, rdcstr()));
        return ToolResult::Success(out);
      }
      if(variants)
      {
        QString query = args.value(QStringLiteral("shaderName")).toString();
        QJsonArray matches;
        int total = 0;
        for(const ResourceDescription &resource : r.GetResources())
        {
          stop = Interrupted(call);
          if(!stop.ok)
            return stop;
          if(resource.type != ResourceType::Shader ||
             (!Str(resource.name).contains(query, Qt::CaseInsensitive) &&
              !Id(resource.resourceId).contains(query, Qt::CaseInsensitive)))
            continue;
          total++;
          if(matches.size() < args.value(QStringLiteral("limit")).toInt(50))
            matches.push_back(QJsonObject{{QStringLiteral("resourceId"), Id(resource.resourceId)},
                                          {QStringLiteral("name"), Str(resource.name)}});
        }
        return ToolResult::Success(
            QJsonObject{{QStringLiteral("query"), query},
                        {QStringLiteral("matchCount"), matches.size()},
                        {QStringLiteral("totalMatches"), total},
                        {QStringLiteral("variants"), matches},
                        {QStringLiteral("truncated"), total > matches.size()},
                        {QStringLiteral("limitation"),
                         QStringLiteral("Name/id candidates only; macro or semantic variant "
                                        "relationships are not inferred.")}});
      }
      auto inspect = [&](uint32_t event, QJsonObject &out) -> ToolResult {
        if(!event || !HasEvent(r.GetRootActions(), event))
          return Fail(ErrorCode::InvalidParams, "eventId does not exist");
        r.SetFrameEvent(event, false);
        const PipeState &pipe = r.GetPipelineState();
        QJsonObject stages;
        QJsonArray available, diag;
        ResourceId filter, pipelineFilter;
        if(args.contains(QStringLiteral("resourceId")) &&
           !ReadId(args.value(QStringLiteral("resourceId")), filter))
          return Fail(ErrorCode::InvalidParams, "Invalid resourceId");
        if(args.contains(QStringLiteral("pipeline")) &&
           !ReadId(args.value(QStringLiteral("pipeline")), pipelineFilter))
          return Fail(ErrorCode::InvalidParams, "Invalid pipeline");
        for(int i = 0; i <= int(ShaderStage::Mesh); i++)
        {
          stop = Interrupted(call);
          if(!stop.ok)
            return stop;
          ShaderStage stage = ShaderStage(i);
          ResourceId shader = pipe.GetShader(stage);
          if(shader == ResourceId())
            continue;
          available.push_back(StageName(stage));
          if(requested != ShaderStage::Count && requested != stage)
            continue;
          if(filter != ResourceId() && shader != filter)
            continue;
          ResourceId pipeline = stage == ShaderStage::Compute ? pipe.GetComputePipelineObject()
                                                              : pipe.GetGraphicsPipelineObject();
          if(pipelineFilter != ResourceId() && pipelineFilter != pipeline)
            return Fail(ErrorCode::InvalidParams, "pipeline contradicts the event binding");
          const ShaderReflection *reflection = pipe.GetShaderReflection(stage);
          if(!reflection)
            return Fail(ErrorCode::Unsupported, "Bound shader reflection unavailable");
          if(args.contains(QStringLiteral("entryPoint")) &&
             args.value(QStringLiteral("entryPoint")).toString() != Str(reflection->entryPoint))
            return Fail(ErrorCode::InvalidParams, "entryPoint contradicts the event binding");
          QJsonObject data = ShaderReflectionToJson(*reflection, includeSource);
          if(diagnostics)
          {
            data[QStringLiteral("debugStatus")] = Str(reflection->debugInfo.debugStatus);
            data[QStringLiteral("debugInfoLoadingLog")] =
                Str(reflection->debugInfo.debugInfoLoadingLog);
          }
          data[QStringLiteral("resourceId")] = Id(shader);
          data[QStringLiteral("pipeline")] = Id(pipeline);
          data[QStringLiteral("readOnlyBindings")] = Descriptors(pipe.GetReadOnlyResources(stage));
          data[QStringLiteral("readWriteBindings")] = Descriptors(pipe.GetReadWriteResources(stage));
          data[QStringLiteral("samplerBindings")] = Descriptors(pipe.GetSamplers(stage));
          if(includeSource)
          {
            data[QStringLiteral("disassembly")] =
                Str(r.DisassembleShader(pipeline, reflection, rdcstr()));
            data[QStringLiteral("sourceIncluded")] = true;
          }
          if(includeCB)
          {
            QJsonArray buffers;
            for(uint32_t b = 0; b < reflection->constantBlocks.size(); b++)
            {
              const ConstantBlock &block = reflection->constantBlocks[b];
              rdcarray<UsedDescriptor> bound;
              for(const UsedDescriptor &descriptor : pipe.GetConstantBlocks(stage))
                if(descriptor.access.index == b)
                  bound.push_back(descriptor);
              if(bound.empty() && !block.bufferBacked)
                bound.push_back(pipe.GetConstantBlock(stage, b, 0));
              for(const UsedDescriptor &descriptor : bound)
              {
                stop = Interrupted(call);
                if(!stop.ok)
                  return stop;
                const uint32_t array = descriptor.access.arrayElement;
                QJsonObject cb{{QStringLiteral("name"), Str(block.name)},
                               {QStringLiteral("index"), double(b)},
                               {QStringLiteral("arrayElement"), double(array)},
                               {QStringLiteral("resourceId"), Id(descriptor.descriptor.resource)},
                               {QStringLiteral("byteOffset"),
                                QString::number(qulonglong(descriptor.descriptor.byteOffset))},
                               {QStringLiteral("byteSize"),
                                QString::number(qulonglong(descriptor.descriptor.byteSize))}};
                QJsonArray variables;
                for(const ShaderVariable &v : r.GetCBufferVariableContents(
                        pipeline, shader, stage, reflection->entryPoint, b,
                        descriptor.descriptor.resource, descriptor.descriptor.byteOffset,
                        descriptor.descriptor.byteSize))
                  variables.push_back(ShaderVariableToJson(v));
                cb[QStringLiteral("variables")] = variables;
                buffers.push_back(cb);
              }
            }
            data[QStringLiteral("constantBuffers")] = buffers;
          }
          stages[StageName(stage)] = data;
          diag.push_back(data);
        }
        if(filter != ResourceId() && stages.isEmpty())
          return Fail(ErrorCode::InvalidParams, "resourceId/stage contradicts the event binding");
        out = QJsonObject{{QStringLiteral("available"), !stages.isEmpty()},
                          {QStringLiteral("eventId"), double(event)},
                          {QStringLiteral("requestedStage"), stageArg},
                          {QStringLiteral("availableStages"), available},
                          {QStringLiteral("shaders"), stages},
                          {QStringLiteral("stages"), stages}};
        if(stages.isEmpty())
          out[QStringLiteral("reason")] =
              QStringLiteral("Requested shader is not bound at this event");
        if(diagnostics)
        {
          out[QStringLiteral("diagnostics")] = diag;
          out[QStringLiteral("limitation")] = QStringLiteral(
              "Captured compiler metadata and debug loading log only; no recompilation and no "
              "inferred historical compiler errors.");
        }
        return ToolResult::Success(out);
      };
      QJsonObject left;
      ToolResult result = inspect(
          uint32_t(args.value(compare ? QStringLiteral("leftEventId") : QStringLiteral("eventId"))
                       .toDouble(currentEvent)),
          left);
      if(!result.ok)
        return result;
      if(!compare)
        return ToolResult::Success(left);
      QJsonObject right;
      result = inspect(uint32_t(args.value(QStringLiteral("rightEventId")).toDouble()), right);
      if(!result.ok)
        return result;
      return ToolResult::Success(QJsonObject{
          {QStringLiteral("available"), left.value(QStringLiteral("available")).toBool() &&
                                            right.value(QStringLiteral("available")).toBool()},
          {QStringLiteral("leftEventId"), args.value(QStringLiteral("leftEventId"))},
          {QStringLiteral("rightEventId"), args.value(QStringLiteral("rightEventId"))},
          {QStringLiteral("equivalentPayload"),
           left.value(QStringLiteral("shaders")) == right.value(QStringLiteral("shaders"))},
          {QStringLiteral("left"), left},
          {QStringLiteral("right"), right},
          {QStringLiteral("note"),
           QStringLiteral("Exact structural comparison of captured reflection/source/bindings; not "
                          "compiler-level semantic equivalence.")}});
    });
  });
  if(!result.ok || !project)
    return result;
  return scanProject();
}

void RegisterShaderTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  auto add = [&](const char *name, const char *description, const QJsonObject &properties,
                 const QJsonArray &required, bool sideEffect = false) {
    QString n = QString::fromUtf8(name);
    registry.Add(
        ToolDefinition{n, n, QString::fromUtf8(description),
                       sideEffect ? ToolEffects::SideEffect : ToolEffects::ReadOnly,
                       ToolRequirements::None, Schema(properties, required)},
        [&adapter, n](const QJsonObject &a, CallContext &c) { return adapter.ShaderTool(n, a, c); });
  };
  QJsonObject event{{QStringLiteral("eventId"), Integer(1, UINT32_MAX)},
                    {QStringLiteral("stage"), Type("string")}};
  add("renderdoc_getShaderSource",
      "Read real bound shader source files, reflection and disassembly. Sensitive gate applies; "
      "event selection is restored.",
      event, QJsonArray{QStringLiteral("eventId")});
  QJsonObject diag = event;
  diag[QStringLiteral("includeSource")] = Type("boolean");
  add("renderdoc_getShaderCompileDiagnostics",
      "Read captured compiler metadata and debug loading diagnostics, without recompiling. "
      "Optional includeSource is sensitive.",
      diag, QJsonArray{QStringLiteral("eventId")});
  add("renderdoc_compareShaders",
      "Compare real shader reflection, source and bindings at two events; no semantic equivalence "
      "claim. Sensitive gate applies.",
      QJsonObject{{QStringLiteral("leftEventId"), Integer(1, UINT32_MAX)},
                  {QStringLiteral("rightEventId"), Integer(1, UINT32_MAX)},
                  {QStringLiteral("stage"), Type("string")}},
      QJsonArray{QStringLiteral("leftEventId"), QStringLiteral("rightEventId")});
  add("renderdoc_findShaderVariants",
      "Search captured shader names/ids with explicit candidate-only semantics and truncation "
      "coverage.",
      QJsonObject{{QStringLiteral("shaderName"), Type("string")},
                  {QStringLiteral("limit"), Integer(1, INT_MAX)}},
      QJsonArray{QStringLiteral("shaderName")});
  QJsonObject flag = Schema(QJsonObject{{QStringLiteral("name"), Type("string")},
                                        {QStringLiteral("value"), Type("string")}},
                            QJsonArray{QStringLiteral("name"), QStringLiteral("value")});
  QJsonObject file = Schema(QJsonObject{{QStringLiteral("filename"), Type("string")},
                                        {QStringLiteral("contents"), Type("string")}},
                            QJsonArray{QStringLiteral("filename"), QStringLiteral("contents")});
  QJsonObject files = Type("array");
  files[QStringLiteral("items")] = file;
  QJsonObject flags = Type("array");
  flags[QStringLiteral("items")] = flag;
  QJsonObject resource = Type("string");
  resource.remove(QStringLiteral("type"));
  resource[QStringLiteral("type")] =
      QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
  QJsonObject edit{{QStringLiteral("resourceId"), resource},
                   {QStringLiteral("shaderStage"), Integer(0, int(ShaderStage::Count) - 1)},
                   {QStringLiteral("sourceEncoding"), Integer(0, int(ShaderEncoding::Count) - 1)},
                   {QStringLiteral("entryPoint"), Type("string")},
                   {QStringLiteral("entryFileIndex"), Integer(0, INT_MAX)},
                   {QStringLiteral("compileFlags"), flags},
                   {QStringLiteral("files"), files}};
  QJsonArray required{QStringLiteral("resourceId"), QStringLiteral("shaderStage"),
                      QStringLiteral("sourceEncoding"), QStringLiteral("entryPoint"),
                      QStringLiteral("files")};
  add("renderdoc_validateShaderEdit",
      "Compile supplied virtual shader files through BuildTargetShader and immediately free the "
      "target. Does not replace or persist. Conditional/macro includes require preprocessed "
      "source.",
      edit, required);
  add("renderdoc_applyShaderEdit",
      "Compile and replace a captured shader in active native replay, publishing CaptureContext "
      "resource mapping. No RDC byte writes. Failed compilation preserves the previous "
      "replacement.",
      edit, required, true);
  QJsonObject strings = Type("array");
  strings[QStringLiteral("items")] = Type("string");
  add("renderdoc_findProjectImplementation",
      "Search explicit native project roots for shader and C#/C++ text/name candidates, optionally "
      "using event reflection source references. Sensitive gate applies; no extension "
      "workspace/runtime required.",
      QJsonObject{{QStringLiteral("eventId"), Integer(1, UINT32_MAX)},
                  {QStringLiteral("shaderName"), Type("string")},
                  {QStringLiteral("passName"), Type("string")},
                  {QStringLiteral("additionalTerms"), strings},
                  {QStringLiteral("projectRoots"), strings},
                  {QStringLiteral("limit"), Integer(1, INT_MAX)}},
      QJsonArray{QStringLiteral("projectRoots")});
}
}
