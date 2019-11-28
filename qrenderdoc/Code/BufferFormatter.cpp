/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2019 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <QApplication>
#include <QRegularExpression>
#include <QtMath>
#include "QRDUtils.h"

struct StructFormatData
{
  ShaderConstant structDef;
  uint32_t offset = 0;
};

GraphicsAPI BufferFormatter::m_API;

ShaderConstant BufferFormatter::ParseFormatString(const QString &formatString, uint64_t maxLen,
                                                  bool tightPacking, QString &errors)
{
  StructFormatData root;
  StructFormatData *cur = &root;

  QMap<QString, StructFormatData> structelems;
  QString lastStruct;

  // regex doesn't account for trailing or preceeding whitespace, or comments

  QRegularExpression regExpr(
      lit("^"                                     // start of the line
          "(row_major\\s+|column_major\\s+)?"     // row_major matrix
          "(rgb\\s+)?"                            // rgb element colourising
          "("                                     // group the options for the type
          "uintten|unormten"                      // R10G10B10A2 types
          "|floateleven"                          // R11G11B10 special type
          "|unormh|unormb"                        // UNORM 16-bit and 8-bit types
          "|snormh|snormb"                        // SNORM 16-bit and 8-bit types
          "|bool"                                 // bool is stored as 4-byte int
          "|byte|short|int|long"                  // signed ints
          "|ubyte|ushort|uint|ulong"              // unsigned ints
          "|xbyte|xshort|xint|xlong"              // hex ints
          "|half|float|double"                    // float types
          "|vec|uvec|ivec"                        // OpenGL vector types
          "|mat|umat|imat"                        // OpenGL matrix types
          ")"                                     // end of the type group
          "([1-9])?"                              // might be a vector
          "(x[1-9])?"                             // or a matrix
          "(\\s+[A-Za-z@_][A-Za-z0-9@_]*)?"       // get identifier name
          "(\\s*\\[[0-9]+\\])?"                   // optional array dimension
          "(\\s*:\\s*[A-Za-z_][A-Za-z0-9_]*)?"    // optional semantic
          "$"));

  bool success = true;
  errors = QString();

  QString text = formatString;

  QRegularExpression c_comments(lit("/\\*[^*]*\\*+(?:[^*/][^*]*\\*+)*/"));
  QRegularExpression cpp_comments(lit("//.*"));
  // remove all comments
  text = text.replace(c_comments, QString()).replace(cpp_comments, QString());
  // ensure braces are forced onto separate lines so we can parse them
  text = text.replace(QLatin1Char('{'), lit("\n{\n")).replace(QLatin1Char('}'), lit("\n}\n"));

  QRegularExpression structDeclRegex(lit("^struct\\s+([A-Za-z_][A-Za-z0-9_]*)$"));
  QRegularExpression structUseRegex(
      lit("^"                                 // start of the line
          "([A-Za-z_][A-Za-z0-9_]*)"          // struct type name
          "\\s+([A-Za-z@_][A-Za-z0-9@_]*)"    // variable name
          "(\\s*\\[[0-9]+\\])?"               // optional array dimension
          "$"));

  // get each line and parse it to determine the format the user wanted
  for(QString &l : text.split(QRegularExpression(lit("[;\n\r]"))))
  {
    QString line = l.trimmed();

    if(line.isEmpty())
      continue;

    if(cur == &root)
    {
      // if we're not in a struct, ignore the braces
      if(line == lit("{") || line == lit("}"))
        continue;
    }
    else
    {
      // if we're in a struct, ignore the opening brace and revert back to root elements when we hit
      // the closing brace. No brace nesting is supported
      if(line == lit("{"))
        continue;

      if(line == lit("}"))
      {
        cur->structDef.type.descriptor.arrayByteStride = cur->offset;

        // struct strides are aligned up to float4 boundary
        if(!tightPacking)
          cur->structDef.type.descriptor.arrayByteStride = (cur->offset + 0xFU) & (~0xFU);

        cur = &root;
        continue;
      }
    }

    if(line.contains(lit("struct")))
    {
      QRegularExpressionMatch match = structDeclRegex.match(line);

      if(match.hasMatch())
      {
        lastStruct = match.captured(1);

        if(structelems.contains(lastStruct))
        {
          errors = tr("Duplicate struct definition: %1\n").arg(lastStruct);
          success = false;
          break;
        }

        cur = &structelems[lastStruct];
        continue;
      }
    }

    ShaderConstant el;

    QRegularExpressionMatch structMatch = structUseRegex.match(line);

    if(structMatch.hasMatch() && structelems.contains(structMatch.captured(1)))
    {
      StructFormatData &structContext = structelems[structMatch.captured(1)];

      QString varName = structMatch.captured(2);

      QString arrayDim = structMatch.captured(3).trimmed();
      uint32_t arrayCount = 1;
      if(!arrayDim.isEmpty())
      {
        arrayDim = arrayDim.mid(1, arrayDim.count() - 2);
        bool ok = false;
        arrayCount = arrayDim.toUInt(&ok);
        if(!ok)
          arrayCount = 1;
      }

      // cbuffer packing rules, structs are always float4 base aligned
      if(!tightPacking)
        cur->offset = (cur->offset + 0xFU) & (~0xFU);

      el = structContext.structDef;
      el.name = varName;
      el.byteOffset = cur->offset;
      el.type.descriptor.elements = arrayCount;

      cur->structDef.type.members.push_back(el);

      // undo the padding after the last struct
      uint32_t padding = el.type.descriptor.arrayByteStride - structContext.offset;

      cur->offset += el.type.descriptor.elements * el.type.descriptor.arrayByteStride - padding;

      continue;
    }

    QRegularExpressionMatch match = regExpr.match(line);

    if(!match.hasMatch())
    {
      errors = tr("Couldn't parse line: %1\n").arg(line);
      success = false;
      break;
    }

    el.name = !match.captured(6).isEmpty() ? match.captured(6).trimmed() : lit("data");

    QString basetype = match.captured(3);
    el.type.descriptor.rowMajorStorage = match.captured(1).trimmed() == lit("row_major");
    el.type.descriptor.displayAsRGB = !match.captured(2).isEmpty();
    QString firstDim = !match.captured(4).isEmpty() ? match.captured(4) : lit("1");
    QString secondDim = !match.captured(5).isEmpty() ? match.captured(5).mid(1) : lit("1");
    QString arrayDim = !match.captured(7).isEmpty() ? match.captured(7).trimmed() : lit("[1]");
    arrayDim = arrayDim.mid(1, arrayDim.count() - 2);

    if(!match.captured(5).isEmpty())
      firstDim.swap(secondDim);

    el.type.descriptor.name = match.captured(1) + match.captured(2) + match.captured(3) +
                              match.captured(4) + match.captured(5);

    ResourceFormatType interpretType = ResourceFormatType::Regular;
    CompType interpretCompType = CompType::Typeless;

    // check for square matrix declarations like 'mat4' and 'mat3'
    if(basetype == lit("mat") && match.captured(5).isEmpty())
      secondDim = firstDim;

    // calculate format
    {
      bool ok = false;

      el.type.descriptor.columns = firstDim.toUInt(&ok);
      if(!ok)
      {
        errors = tr("Invalid vector dimension on line: %1\n").arg(line);
        success = false;
        break;
      }

      el.type.descriptor.elements = qMax(1U, arrayDim.toUInt(&ok));
      if(!ok)
        el.type.descriptor.elements = 1;

      el.type.descriptor.rows = qMax(1U, secondDim.toUInt(&ok));
      if(!ok)
      {
        errors = tr("Invalid matrix second dimension on line: %1\n").arg(line);
        success = false;
        break;
      }

      // vectors are never marked as row major
      if(el.type.descriptor.rows == 1)
        el.type.descriptor.rowMajorStorage = false;

      if(basetype == lit("bool"))
      {
        el.type.descriptor.type = VarType::UInt;
      }
      else if(basetype == lit("byte"))
      {
        el.type.descriptor.type = VarType::SByte;
      }
      else if(basetype == lit("ubyte") || basetype == lit("xbyte"))
      {
        el.type.descriptor.type = VarType::UByte;
      }
      else if(basetype == lit("short"))
      {
        el.type.descriptor.type = VarType::SShort;
      }
      else if(basetype == lit("ushort") || basetype == lit("xshort"))
      {
        el.type.descriptor.type = VarType::UShort;
      }
      else if(basetype == lit("long"))
      {
        el.type.descriptor.type = VarType::SLong;
      }
      else if(basetype == lit("ulong") || basetype == lit("xlong"))
      {
        el.type.descriptor.type = VarType::ULong;
      }
      else if(basetype == lit("int") || basetype == lit("ivec") || basetype == lit("imat"))
      {
        el.type.descriptor.type = VarType::SInt;
      }
      else if(basetype == lit("uint") || basetype == lit("xint") || basetype == lit("uvec") ||
              basetype == lit("umat"))
      {
        el.type.descriptor.type = VarType::UInt;
      }
      else if(basetype == lit("half"))
      {
        el.type.descriptor.type = VarType::Half;
      }
      else if(basetype == lit("float") || basetype == lit("vec") || basetype == lit("mat"))
      {
        el.type.descriptor.type = VarType::Float;
      }
      else if(basetype == lit("double"))
      {
        el.type.descriptor.type = VarType::Double;
      }
      else if(basetype == lit("unormh"))
      {
        el.type.descriptor.type = VarType::UShort;
        interpretCompType = CompType::UNorm;
      }
      else if(basetype == lit("unormb"))
      {
        el.type.descriptor.type = VarType::UByte;
        interpretCompType = CompType::UNorm;
      }
      else if(basetype == lit("snormh"))
      {
        el.type.descriptor.type = VarType::SShort;
        interpretCompType = CompType::SNorm;
      }
      else if(basetype == lit("snormb"))
      {
        el.type.descriptor.type = VarType::SByte;
        interpretCompType = CompType::SNorm;
      }
      else if(basetype == lit("uintten"))
      {
        el.type.descriptor.type = VarType::UInt;
        interpretType = ResourceFormatType::R10G10B10A2;
      }
      else if(basetype == lit("unormten"))
      {
        el.type.descriptor.type = VarType::UInt;
        interpretCompType = CompType::UNorm;
        interpretType = ResourceFormatType::R10G10B10A2;
      }
      else if(basetype == lit("floateleven"))
      {
        el.type.descriptor.type = VarType::Float;
        interpretCompType = CompType::Float;
        interpretType = ResourceFormatType::R11G11B10;
      }
      else
      {
        errors = tr("Unrecognised basic type on line: %1\n").arg(line);
        success = false;
        break;
      }
    }

    if(basetype == lit("xlong") || basetype == lit("xint") || basetype == lit("xshort") ||
       basetype == lit("xbyte"))
      el.type.descriptor.displayAsHex = true;

    SetInterpretedResourceFormat(el, interpretType, interpretCompType);

    ResourceFormat fmt = GetInterpretedResourceFormat(el);

    // normally the array stride is the size of an element
    el.type.descriptor.arrayByteStride = el.type.descriptor.matrixByteStride = fmt.ElementSize();

    uint32_t padding = 0;

    // for matrices, it's the size of an element times the number of rows
    if(el.type.descriptor.rows > 1)
    {
      // if we're cbuffer packing, matrix row/columns are always 16-bytes apart
      if(!tightPacking)
      {
        padding = 16 - el.type.descriptor.matrixByteStride;
        el.type.descriptor.matrixByteStride = 16;
      }

      uint8_t majorDim =
          el.type.descriptor.rowMajorStorage ? el.type.descriptor.rows : el.type.descriptor.columns;

      // total matrix size is
      el.type.descriptor.arrayByteStride = el.type.descriptor.matrixByteStride * majorDim;
    }

    // cbuffer packing rules
    if(!tightPacking)
    {
      if(el.type.descriptor.elements == 1)
      {
        // always float aligned
        el.type.descriptor.arrayByteStride = (el.type.descriptor.arrayByteStride + 3U) & (~3U);

        // elements can't cross float4 boundaries, nudge up if this was the case
        if(cur->offset / 16 != (cur->offset + el.type.descriptor.arrayByteStride - 1) / 16)
        {
          cur->offset = (cur->offset + 0xFU) & (~0xFU);
        }
      }
      else
      {
        // arrays always have elements float4 aligned
        uint32_t paddedStride = (el.type.descriptor.arrayByteStride + 0xFU) & (~0xFU);

        padding += paddedStride - el.type.descriptor.arrayByteStride;

        el.type.descriptor.arrayByteStride = paddedStride;

        // and always aligned at float4 boundary
        if(cur->offset % 16 != 0)
        {
          cur->offset = (cur->offset + 0xFU) & (~0xFU);
        }
      }
    }

    el.byteOffset = cur->offset;

    cur->structDef.type.members.push_back(el);

    cur->offset += el.type.descriptor.arrayByteStride * el.type.descriptor.elements - padding;
  }

  // if we succeeded parsing but didn't get any root elements, use the last defined struct as the
  // definition
  if(success && root.structDef.type.members.isEmpty() && !lastStruct.isEmpty())
    root = structelems[lastStruct];

  root.structDef.type.descriptor.arrayByteStride = root.offset;

  // struct strides are aligned up to float4 boundary
  if(!tightPacking)
    root.structDef.type.descriptor.arrayByteStride = (root.offset + 0xFU) & (~0xFU);

  if(!success || root.structDef.type.members.isEmpty())
  {
    root.structDef.type.members.clear();

    ShaderConstant el;
    el.byteOffset = 0;
    el.type.descriptor.displayAsHex = true;
    el.name = "data";
    el.type.descriptor.type = VarType::UInt;
    el.type.descriptor.columns = 4;

    if(maxLen > 0 && maxLen < 16)
      el.type.descriptor.columns = 1;
    if(maxLen > 0 && maxLen < 4)
      el.type.descriptor.type = VarType::UByte;

    SetInterpretedResourceFormat(el, ResourceFormatType::Regular, CompType::Typeless);

    root.structDef.type.members.push_back(el);
  }

  return root.structDef;
}

QString BufferFormatter::GetTextureFormatString(const TextureDescription &tex)
{
  QString baseType;

  QString varName = lit("pixels");

  uint32_t w = tex.width;

  switch(tex.format.type)
  {
    case ResourceFormatType::BC1:
    case ResourceFormatType::BC2:
    case ResourceFormatType::BC3:
    case ResourceFormatType::BC4:
    case ResourceFormatType::BC5:
    case ResourceFormatType::BC6:
    case ResourceFormatType::BC7:
    case ResourceFormatType::ETC2:
    case ResourceFormatType::EAC:
    case ResourceFormatType::ASTC:
    case ResourceFormatType::PVRTC:
      varName = lit("block");
      // display a 4x4 block at a time
      w /= 4;
    default: break;
  }

  switch(tex.format.type)
  {
    case ResourceFormatType::Regular:
    {
      if(tex.format.compByteWidth == 1)
      {
        if(tex.format.compType == CompType::UNorm || tex.format.compType == CompType::UNormSRGB)
          baseType = lit("unormb");
        else if(tex.format.compType == CompType::SNorm)
          baseType = lit("snormb");
        else if(tex.format.compType == CompType::SInt)
          baseType = lit("byte");
        else
          baseType = lit("ubyte");
      }
      else if(tex.format.compByteWidth == 2)
      {
        if(tex.format.compType == CompType::UNorm || tex.format.compType == CompType::UNormSRGB)
          baseType = lit("unormh");
        else if(tex.format.compType == CompType::SNorm)
          baseType = lit("snormh");
        else if(tex.format.compType == CompType::Float)
          baseType = lit("half");
        else if(tex.format.compType == CompType::SInt)
          baseType = lit("short");
        else
          baseType = lit("ushort");
      }
      else if(tex.format.compByteWidth == 4)
      {
        if(tex.format.compType == CompType::Float)
          baseType = lit("float");
        else if(tex.format.compType == CompType::SInt)
          baseType = lit("int");
        else
          baseType = lit("uint");
      }
      else
      {
        if(tex.format.compType == CompType::Float || tex.format.compType == CompType::Double)
          baseType = lit("double");
        else if(tex.format.compType == CompType::SInt)
          baseType = lit("long");
        else
          baseType = lit("ulong");
      }

      baseType = QFormatStr("rgb %1%2").arg(baseType).arg(tex.format.compCount);

      break;
    }
    // 2x4 byte block, for 64-bit block formats
    case ResourceFormatType::BC1:
    case ResourceFormatType::BC4:
    case ResourceFormatType::ETC2:
    case ResourceFormatType::EAC:
    case ResourceFormatType::PVRTC:
      baseType = lit("row_major xint2x1");
      break;
    // 4x4 byte block, for 128-bit block formats
    case ResourceFormatType::BC2:
    case ResourceFormatType::BC3:
    case ResourceFormatType::BC5:
    case ResourceFormatType::BC6:
    case ResourceFormatType::BC7:
    case ResourceFormatType::ASTC: baseType = lit("row_major xint4x1"); break;
    case ResourceFormatType::R10G10B10A2: baseType = lit("uintten"); break;
    case ResourceFormatType::R11G11B10: baseType = lit("rgb floateleven"); break;
    case ResourceFormatType::R5G6B5:
    case ResourceFormatType::R5G5B5A1: baseType = lit("xshort"); break;
    case ResourceFormatType::R9G9B9E5: baseType = lit("xint"); break;
    case ResourceFormatType::R4G4B4A4: baseType = lit("xbyte2"); break;
    case ResourceFormatType::R4G4: baseType = lit("xbyte"); break;
    case ResourceFormatType::D16S8:
    case ResourceFormatType::D24S8:
    case ResourceFormatType::D32S8:
    case ResourceFormatType::YUV8: baseType = lit("xbyte4"); break;
    case ResourceFormatType::YUV10:
    case ResourceFormatType::YUV12:
    case ResourceFormatType::YUV16: baseType = lit("xshort4"); break;
    case ResourceFormatType::A8:
    case ResourceFormatType::S8:
    case ResourceFormatType::Undefined: baseType = lit("xbyte"); break;
  }

  if(tex.type == TextureType::Buffer)
    return QFormatStr("%1 %2;").arg(baseType).arg(varName);

  return QFormatStr("%1 %2[%3];").arg(baseType).arg(varName).arg(w);
}

QString BufferFormatter::GetBufferFormatString(const ShaderResource &res,
                                               const ResourceFormat &viewFormat,
                                               uint64_t &baseByteOffset)
{
  QString format;

  if(!res.variableType.members.empty())
  {
    if(m_API == GraphicsAPI::Vulkan || m_API == GraphicsAPI::OpenGL)
    {
      const rdcarray<ShaderConstant> &members = res.variableType.members;

      format += QFormatStr("struct %1\n{\n").arg(res.name);

      // GL/Vulkan allow fixed-sized members before the array-of-structs. This can't be
      // represented in a buffer format so we skip it
      if(members.count() > 1)
      {
        format += tr("    // members skipped as they are fixed size:\n");
        baseByteOffset += members.back().byteOffset;
      }

      QString varTypeName;
      QString comment = lit("// ");
      for(int i = 0; i < members.count(); i++)
      {
        QString arraySize;
        if(members[i].type.descriptor.elements > 1)
          arraySize = QFormatStr("[%1]").arg(members[i].type.descriptor.elements);

        varTypeName = members[i].type.descriptor.name;

        if(i + 1 == members.count())
        {
          comment = arraySize = QString();

          if(members.count() > 1)
            format +=
                lit("    // final array struct @ byte offset %1\n").arg(members.back().byteOffset);

          // give GL nameless structs a better name
          if(varTypeName.isEmpty() || varTypeName == lit("struct"))
            varTypeName = lit("root_struct");
        }

        format +=
            QFormatStr("    %1%2 %3%4;\n").arg(comment).arg(varTypeName).arg(members[i].name).arg(arraySize);
      }

      format += lit("}");

      // if the last member is a struct, declare it
      if(!members.back().type.members.isEmpty())
      {
        format = DeclareStruct(varTypeName, members.back().type.members,
                               members.back().type.descriptor.arrayByteStride) +
                 lit("\n") + format;
      }
    }
    else
    {
      format = DeclareStruct(res.variableType.descriptor.name, res.variableType.members, 0);
    }
  }
  else
  {
    const auto &desc = res.variableType.descriptor;

    if(viewFormat.type == ResourceFormatType::Undefined)
    {
      if(desc.type == VarType::Unknown)
      {
        format = desc.name;
      }
      else
      {
        if(desc.rowMajorStorage)
          format += lit("row_major ");

        format += ToQStr(desc.type);
        if(desc.rows > 1 && desc.columns > 1)
          format += QFormatStr("%1x%2").arg(desc.rows).arg(desc.columns);
        else if(desc.columns > 1)
          format += QString::number(desc.columns);

        if(!desc.name.empty())
          format += lit(" ") + desc.name;

        if(desc.elements > 1)
          format += QFormatStr("[%1]").arg(desc.elements);
      }
    }
    else
    {
      if(viewFormat.type == ResourceFormatType::R10G10B10A2)
      {
        if(viewFormat.compType == CompType::UInt)
          format = lit("uintten");
        if(viewFormat.compType == CompType::UNorm)
          format = lit("unormten");
      }
      else if(viewFormat.type == ResourceFormatType::R11G11B10)
      {
        format = lit("floateleven");
      }
      else
      {
        switch(viewFormat.compByteWidth)
        {
          case 1:
          {
            if(viewFormat.compType == CompType::UNorm || viewFormat.compType == CompType::UNormSRGB)
              format = lit("unormb");
            if(viewFormat.compType == CompType::SNorm)
              format = lit("snormb");
            if(viewFormat.compType == CompType::UInt)
              format = lit("ubyte");
            if(viewFormat.compType == CompType::SInt)
              format = lit("byte");
            break;
          }
          case 2:
          {
            if(viewFormat.compType == CompType::UNorm || viewFormat.compType == CompType::UNormSRGB)
              format = lit("unormh");
            if(viewFormat.compType == CompType::SNorm)
              format = lit("snormh");
            if(viewFormat.compType == CompType::UInt)
              format = lit("ushort");
            if(viewFormat.compType == CompType::SInt)
              format = lit("short");
            if(viewFormat.compType == CompType::Float)
              format = lit("half");
            break;
          }
          case 4:
          {
            if(viewFormat.compType == CompType::UNorm || viewFormat.compType == CompType::UNormSRGB)
              format = lit("unormf");
            if(viewFormat.compType == CompType::SNorm)
              format = lit("snormf");
            if(viewFormat.compType == CompType::UInt)
              format = lit("uint");
            if(viewFormat.compType == CompType::SInt)
              format = lit("int");
            if(viewFormat.compType == CompType::Float)
              format = lit("float");
            break;
          }
        }

        format += QString::number(viewFormat.compCount);
      }
    }
  }

  return format;
}

QString BufferFormatter::DeclarePaddingBytes(uint32_t bytes)
{
  if(bytes == 0)
    return QString();

  QString ret;

  if(bytes > 4)
  {
    ret += lit("xint pad[%1];").arg(bytes / 4);

    bytes = bytes % 4;
  }

  if(bytes == 4)
    ret += lit("xint pad;");
  else if(bytes == 3)
    ret += lit("xshort pad; xbyte pad;");
  else if(bytes == 2)
    ret += lit("xshort pad;");
  else if(bytes == 1)
    ret += lit("xbyte pad;");

  return ret + lit("\n");
}

QString BufferFormatter::DeclareStruct(QList<QString> &declaredStructs, const QString &name,
                                       const rdcarray<ShaderConstant> &members,
                                       uint32_t requiredByteStride)
{
  QString ret;

  ret = lit("struct %1\n{\n").arg(name);

  for(int i = 0; i < members.count(); i++)
  {
    QString arraySize;
    if(members[i].type.descriptor.elements > 1)
      arraySize = QFormatStr("[%1]").arg(members[i].type.descriptor.elements);

    QString varTypeName = members[i].type.descriptor.name;

    if(!members[i].type.members.isEmpty())
    {
      // GL structs don't give us typenames (boo!) so give them unique names. This will mean some
      // structs get duplicated if they're used in multiple places, but not much we can do about
      // that.
      if(varTypeName.isEmpty() || varTypeName == lit("struct"))
        varTypeName = lit("anon%1").arg(declaredStructs.size());

      if(!declaredStructs.contains(varTypeName))
      {
        declaredStructs.push_back(varTypeName);
        ret = DeclareStruct(declaredStructs, varTypeName, members[i].type.members,
                            members[i].type.descriptor.arrayByteStride) +
              lit("\n") + ret;
      }
    }

    ret += QFormatStr("    %1 %2%3;\n").arg(varTypeName).arg(members[i].name).arg(arraySize);
  }

  if(requiredByteStride > 0)
  {
    uint32_t structStart = 0;

    const ShaderConstant *lastChild = &members.back();

    structStart += lastChild->byteOffset;
    structStart += (qMax(lastChild->type.descriptor.elements, 1U) - 1) *
                   lastChild->type.descriptor.arrayByteStride;
    while(!lastChild->type.members.isEmpty())
    {
      lastChild = &lastChild->type.members.back();
      structStart += lastChild->byteOffset;
      structStart += (qMax(lastChild->type.descriptor.elements, 1U) - 1) *
                     lastChild->type.descriptor.arrayByteStride;
    }

    uint32_t size = lastChild->type.descriptor.rows * lastChild->type.descriptor.columns;
    if(lastChild->type.descriptor.type == VarType::Double ||
       lastChild->type.descriptor.type == VarType::ULong ||
       lastChild->type.descriptor.type == VarType::SLong)
      size *= 8;
    else if(lastChild->type.descriptor.type == VarType::Float ||
            lastChild->type.descriptor.type == VarType::UInt ||
            lastChild->type.descriptor.type == VarType::SInt)
      size *= 4;
    else if(lastChild->type.descriptor.type == VarType::Half ||
            lastChild->type.descriptor.type == VarType::UShort ||
            lastChild->type.descriptor.type == VarType::SShort)
      size *= 2;

    if(lastChild->type.descriptor.elements > 1)
      size *= lastChild->type.descriptor.elements;

    uint32_t padBytes = requiredByteStride - (structStart + size);

    if(padBytes > 0)
      ret += lit("    ") + DeclarePaddingBytes(padBytes);
  }

  ret += lit("}\n");

  return ret;
}

QString BufferFormatter::DeclareStruct(const QString &name, const rdcarray<ShaderConstant> &members,
                                       uint32_t requiredByteStride)
{
  QList<QString> declaredStructs;
  return DeclareStruct(declaredStructs, name, members, requiredByteStride);
}

void SetInterpretedResourceFormat(ShaderConstant &elem, ResourceFormatType interpretType,
                                  CompType interpretCompType)
{
  static_assert(sizeof(elem.defaultValue) > 2,
                "ShaderConstant::defaultValue has changed, check packing");

  // packing must match GetInterpretedResourceFormat below
  elem.defaultValue = (uint64_t(interpretType) << 8) | (uint64_t(interpretCompType) << 0);
}

ResourceFormat GetInterpretedResourceFormat(const ShaderConstant &elem)
{
  // packing must match SetInterpretedResourceFormat above
  ResourceFormatType interpretType = ResourceFormatType((elem.defaultValue >> 8) & 0xff);
  CompType interpretCompType = CompType((elem.defaultValue >> 0) & 0xff);

  ResourceFormat format;
  format.type = interpretType;

  switch(elem.type.descriptor.type)
  {
    case VarType::Half:
    case VarType::Float: format.compType = CompType::Float; break;
    case VarType::Double: format.compType = CompType::Double; break;
    case VarType::SInt:
    case VarType::SShort:
    case VarType::SLong:
    case VarType::SByte: format.compType = CompType::SInt; break;
    case VarType::UInt:
    case VarType::UShort:
    case VarType::ULong:
    case VarType::UByte: format.compType = CompType::UInt; break;
    default: format.compType = CompType::UInt; break;
  }

  if(interpretCompType != CompType::Typeless)
    format.compType = interpretCompType;

  format.compByteWidth = VarTypeByteSize(elem.type.descriptor.type);

  if(elem.type.descriptor.rowMajorStorage || elem.type.descriptor.rows == 1)
    format.compCount = elem.type.descriptor.columns;
  else
    format.compCount = elem.type.descriptor.rows;

  return format;
}

static void FillShaderVarData(ShaderVariable &var, const ShaderConstant &elem, const byte *data,
                              const byte *end)
{
  int src = 0;

  uint32_t outerCount = elem.type.descriptor.rows;
  uint32_t innerCount = elem.type.descriptor.columns;

  bool colMajor = false;

  if(!elem.type.descriptor.rowMajorStorage && outerCount > 1)
  {
    colMajor = true;
    std::swap(outerCount, innerCount);
  }

  QVariantList objs = GetVariants(GetInterpretedResourceFormat(elem), outerCount,
                                  elem.type.descriptor.matrixByteStride, data, end);

  if(objs.isEmpty())
  {
    var.name = "-";
    memset(var.value.dv, 0, sizeof(var.value.dv));
    return;
  }

  for(uint32_t outer = 0; outer < outerCount; outer++)
  {
    for(uint32_t inner = 0; inner < innerCount; inner++)
    {
      uint32_t dst = outer * elem.type.descriptor.columns + inner;

      if(colMajor)
        dst = inner * elem.type.descriptor.columns + outer;

      const QVariant &o = objs[src];

      src++;

      if(var.type == VarType::Double)
        var.value.dv[dst] = o.toDouble();
      if(var.type == VarType::Float || var.type == VarType::Half)
        var.value.fv[dst] = o.toFloat();
      else if(var.type == VarType::ULong)
        var.value.u64v[dst] = o.toULongLong();
      else if(var.type == VarType::SLong)
        var.value.s64v[dst] = o.toLongLong();
      else if(var.type == VarType::UInt || var.type == VarType::UShort || var.type == VarType::UByte)
        var.value.uv[dst] = o.toUInt();
      else if(var.type == VarType::SInt || var.type == VarType::SShort || var.type == VarType::SByte)
        var.value.iv[dst] = o.toInt();
      else
        var.value.fv[dst] = o.toFloat();
    }
  }
}

ShaderVariable InterpretShaderVar(const ShaderConstant &elem, const byte *data, const byte *end)
{
  ShaderVariable ret;

  ret.name = elem.name;
  ret.type = elem.type.descriptor.type;
  ret.columns = qMin(elem.type.descriptor.columns, uint8_t(4));
  ret.rows = qMin(elem.type.descriptor.rows, uint8_t(4));
  ret.isStruct = !elem.type.members.isEmpty();

  ret.displayAsHex = elem.type.descriptor.displayAsHex;
  ret.rowMajor = elem.type.descriptor.rowMajorStorage;

  if(!elem.type.members.isEmpty())
  {
    ret.rows = ret.columns = 0;

    if(elem.type.descriptor.elements > 1)
    {
      rdcarray<ShaderVariable> arrayElements;

      for(uint32_t a = 0; a < elem.type.descriptor.elements; a++)
      {
        rdcarray<ShaderVariable> members;

        for(size_t m = 0; m < elem.type.members.size(); m++)
        {
          const ShaderConstant &member = elem.type.members[m];

          members.push_back(InterpretShaderVar(member, data + member.byteOffset, end));
        }

        arrayElements.push_back(ret);
        arrayElements.back().name = QFormatStr("%1[%2]").arg(ret.name).arg(a);
        arrayElements.back().members = members;

        data += elem.type.descriptor.arrayByteStride;
      }

      ret.isStruct = false;
      ret.members = arrayElements;
    }
    else
    {
      rdcarray<ShaderVariable> members;

      for(size_t m = 0; m < elem.type.members.size(); m++)
      {
        const ShaderConstant &member = elem.type.members[m];

        members.push_back(InterpretShaderVar(member, data + member.byteOffset, end));
      }

      ret.members = members;
    }
  }
  else if(elem.type.descriptor.elements > 1)
  {
    rdcarray<ShaderVariable> arrayElements;

    for(uint32_t a = 0; a < elem.type.descriptor.elements; a++)
    {
      arrayElements.push_back(ret);
      arrayElements.back().name = QFormatStr("%1[%2]").arg(ret.name).arg(a);
      FillShaderVarData(arrayElements.back(), elem, data, end);
      data += elem.type.descriptor.arrayByteStride;
    }

    ret.rows = ret.columns = 0;
    ret.members = arrayElements;
  }
  else
  {
    FillShaderVarData(ret, elem, data, end);
  }

  return ret;
}

static QVariant interpret(const ResourceFormat &f, uint16_t comp)
{
  if(f.compByteWidth != 2 || f.compType == CompType::Float)
    return QVariant();

  if(f.compType == CompType::SInt)
  {
    return (int16_t)comp;
  }
  else if(f.compType == CompType::UInt)
  {
    return comp;
  }
  else if(f.compType == CompType::SScaled)
  {
    return (float)((int16_t)comp);
  }
  else if(f.compType == CompType::UScaled)
  {
    return (float)comp;
  }
  else if(f.compType == CompType::UNorm || f.compType == CompType::UNormSRGB)
  {
    return (float)comp / (float)0xffff;
  }
  else if(f.compType == CompType::SNorm)
  {
    int16_t cast = (int16_t)comp;

    float ret = -1.0f;

    if(cast == -32768)
      ret = -1.0f;
    else
      ret = ((float)cast) / 32767.0f;

    return ret;
  }

  return QVariant();
}

static QVariant interpret(const ResourceFormat &f, byte comp)
{
  if(f.compByteWidth != 1 || f.compType == CompType::Float)
    return QVariant();

  if(f.compType == CompType::SInt)
  {
    return (int8_t)comp;
  }
  else if(f.compType == CompType::UInt)
  {
    return comp;
  }
  else if(f.compType == CompType::SScaled)
  {
    return (float)((int8_t)comp);
  }
  else if(f.compType == CompType::UScaled)
  {
    return (float)comp;
  }
  else if(f.compType == CompType::UNorm || f.compType == CompType::UNormSRGB)
  {
    return ((float)comp) / 255.0f;
  }
  else if(f.compType == CompType::SNorm)
  {
    int8_t cast = (int8_t)comp;

    float ret = -1.0f;

    if(cast == -128)
      ret = -1.0f;
    else
      ret = ((float)cast) / 127.0f;

    return ret;
  }

  return QVariant();
}

template <typename T>
inline T readObj(const byte *&data, const byte *end, bool &ok)
{
  if(data + sizeof(T) > end)
  {
    ok = false;
    return T();
  }

  T ret = *(T *)data;

  data += sizeof(T);

  return ret;
}

QVariantList GetVariants(ResourceFormat rowFormat, uint32_t rowCount, uint32_t rowByteStride,
                         const byte *&data, const byte *end)
{
  QVariantList ret;

  bool ok = true;

  if(rowFormat.type == ResourceFormatType::R5G5B5A1)
  {
    uint16_t packed = readObj<uint16_t>(data, end, ok);

    ret.push_back((float)((packed >> 0) & 0x1f) / 31.0f);
    ret.push_back((float)((packed >> 5) & 0x1f) / 31.0f);
    ret.push_back((float)((packed >> 10) & 0x1f) / 31.0f);
    ret.push_back(((packed & 0x8000) > 0) ? 1.0f : 0.0f);

    if(rowFormat.BGRAOrder())
    {
      QVariant tmp = ret[2];
      ret[2] = ret[0];
      ret[0] = tmp;
    }
  }
  else if(rowFormat.type == ResourceFormatType::R5G6B5)
  {
    uint16_t packed = readObj<uint16_t>(data, end, ok);

    ret.push_back((float)((packed >> 0) & 0x1f) / 31.0f);
    ret.push_back((float)((packed >> 5) & 0x3f) / 63.0f);
    ret.push_back((float)((packed >> 11) & 0x1f) / 31.0f);

    if(rowFormat.BGRAOrder())
    {
      QVariant tmp = ret[2];
      ret[2] = ret[0];
      ret[0] = tmp;
    }
  }
  else if(rowFormat.type == ResourceFormatType::R4G4B4A4)
  {
    uint16_t packed = readObj<uint16_t>(data, end, ok);

    ret.push_back((float)((packed >> 0) & 0xf) / 15.0f);
    ret.push_back((float)((packed >> 4) & 0xf) / 15.0f);
    ret.push_back((float)((packed >> 8) & 0xf) / 15.0f);
    ret.push_back((float)((packed >> 12) & 0xf) / 15.0f);

    if(rowFormat.BGRAOrder())
    {
      QVariant tmp = ret[2];
      ret[2] = ret[0];
      ret[0] = tmp;
    }
  }
  else if(rowFormat.type == ResourceFormatType::R10G10B10A2)
  {
    // allow for vectors of this format - for raw buffer viewer
    for(int i = 0; i < int(rowFormat.compCount / 4); i++)
    {
      uint32_t packed = readObj<uint32_t>(data, end, ok);

      uint32_t r = (packed >> 0) & 0x3ff;
      uint32_t g = (packed >> 10) & 0x3ff;
      uint32_t b = (packed >> 20) & 0x3ff;
      uint32_t a = (packed >> 30) & 0x003;

      if(rowFormat.BGRAOrder())
      {
        uint32_t tmp = b;
        b = r;
        r = tmp;
      }

      if(rowFormat.compType == CompType::UInt)
      {
        ret.push_back(r);
        ret.push_back(g);
        ret.push_back(b);
        ret.push_back(a);
      }
      else if(rowFormat.compType == CompType::UScaled)
      {
        ret.push_back((float)r);
        ret.push_back((float)g);
        ret.push_back((float)b);
        ret.push_back((float)a);
      }
      else if(rowFormat.compType == CompType::SInt || rowFormat.compType == CompType::SScaled ||
              rowFormat.compType == CompType::SNorm)
      {
        int ir, ig, ib, ia;

        // interpret RGB as 10-bit signed integers
        if(r <= 511)
          ir = (int)r;
        else
          ir = ((int)r) - 1024;

        if(g <= 511)
          ig = (int)g;
        else
          ig = ((int)g) - 1024;

        if(b <= 511)
          ib = (int)b;
        else
          ib = ((int)b) - 1024;

        // 2-bit signed integer
        if(a <= 1)
          ia = (int)a;
        else
          ia = ((int)a) - 4;

        if(rowFormat.compType == CompType::SInt)
        {
          ret.push_back(ir);
          ret.push_back(ig);
          ret.push_back(ib);
          ret.push_back(ia);
        }
        else if(rowFormat.compType == CompType::SScaled)
        {
          ret.push_back((float)ir);
          ret.push_back((float)ig);
          ret.push_back((float)ib);
          ret.push_back((float)ia);
        }
        else if(rowFormat.compType == CompType::SNorm)
        {
          if(ir == -512)
            ir = -511;
          if(ig == -512)
            ig = -511;
          if(ib == -512)
            ib = -511;
          if(ia == -2)
            ia = -1;

          ret.push_back((float)ir / 511.0f);
          ret.push_back((float)ig / 511.0f);
          ret.push_back((float)ib / 511.0f);
          ret.push_back((float)ia / 1.0f);
        }
      }
      else
      {
        ret.push_back((float)r / 1023.0f);
        ret.push_back((float)g / 1023.0f);
        ret.push_back((float)b / 1023.0f);
        ret.push_back((float)a / 3.0f);
      }
    }
  }
  else if(rowFormat.type == ResourceFormatType::R11G11B10)
  {
    uint32_t packed = readObj<uint32_t>(data, end, ok);

    uint32_t mantissas[] = {
        (packed >> 0) & 0x3f, (packed >> 11) & 0x3f, (packed >> 22) & 0x1f,
    };
    int32_t exponents[] = {
        int32_t(packed >> 6) & 0x1f, int32_t(packed >> 17) & 0x1f, int32_t(packed >> 27) & 0x1f,
    };
    static const uint32_t leadbit[] = {
        0x40, 0x40, 0x20,
    };

    for(int i = 0; i < 3; i++)
    {
      if(mantissas[i] == 0 && exponents[i] == 0)
      {
        ret.push_back((float)0.0f);
      }
      else
      {
        if(exponents[i] == 0x1f)
        {
          // no sign bit, can't be negative infinity
          if(mantissas[i] == 0)
            ret.push_back((float)qInf());
          else
            ret.push_back((float)qQNaN());
        }
        else if(exponents[i] != 0)
        {
          // normal value, add leading bit
          uint32_t combined = leadbit[i] | mantissas[i];

          // calculate value
          ret.push_back(((float)combined / (float)leadbit[i]) *
                        qPow(2.0f, (float)exponents[i] - 15.0f));
        }
        else if(exponents[i] == 0)
        {
          // we know xMantissa isn't 0 also, or it would have been caught above so
          // this is a subnormal value, pretend exponent is 1 and don't add leading bit

          ret.push_back(((float)mantissas[i] / (float)leadbit[i]) * qPow(2.0f, (float)1.0f - 15.0f));
        }
      }
    }
  }
  else
  {
    const byte *base = data;

    for(uint32_t row = 0; row < qMax(rowCount, 1U); row++)
    {
      const byte *rowStart = base + row * rowByteStride;

      if(data < rowStart)
        data = rowStart;

      for(int i = 0; i < rowFormat.compCount; i++)
      {
        if(rowFormat.compType == CompType::Float)
        {
          if(rowFormat.compByteWidth == 8)
            ret.push_back(readObj<double>(data, end, ok));
          else if(rowFormat.compByteWidth == 4)
            ret.push_back(readObj<float>(data, end, ok));
          else if(rowFormat.compByteWidth == 2)
            ret.push_back(RENDERDOC_HalfToFloat(readObj<uint16_t>(data, end, ok)));
        }
        else if(rowFormat.compType == CompType::SInt)
        {
          if(rowFormat.compByteWidth == 8)
            ret.push_back((qlonglong)readObj<int64_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 4)
            ret.push_back((int)readObj<int32_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 2)
            ret.push_back((int)readObj<int16_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 1)
            ret.push_back((int)readObj<int8_t>(data, end, ok));
        }
        else if(rowFormat.compType == CompType::UInt)
        {
          if(rowFormat.compByteWidth == 8)
            ret.push_back((qulonglong)readObj<uint64_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 4)
            ret.push_back((uint32_t)readObj<uint32_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 2)
            ret.push_back((uint32_t)readObj<uint16_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 1)
            ret.push_back((uint32_t)readObj<uint8_t>(data, end, ok));
        }
        else if(rowFormat.compType == CompType::UScaled)
        {
          if(rowFormat.compByteWidth == 4)
            ret.push_back((float)readObj<uint32_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 2)
            ret.push_back((float)readObj<uint16_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 1)
            ret.push_back((float)readObj<uint8_t>(data, end, ok));
        }
        else if(rowFormat.compType == CompType::SScaled)
        {
          if(rowFormat.compByteWidth == 4)
            ret.push_back((float)readObj<int32_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 2)
            ret.push_back((float)readObj<int16_t>(data, end, ok));
          else if(rowFormat.compByteWidth == 1)
            ret.push_back((float)readObj<int8_t>(data, end, ok));
        }
        else if(rowFormat.compType == CompType::Depth)
        {
          if(rowFormat.compByteWidth == 4)
          {
            // 32-bit depth is native floats
            ret.push_back(readObj<float>(data, end, ok));
          }
          else if(rowFormat.compByteWidth == 3)
          {
            // 32-bit depth is normalised, masked against non-stencil bits
            uint32_t f = readObj<uint32_t>(data, end, ok);
            f &= 0x00ffffff;
            ret.push_back((float)f / (float)0x00ffffff);
          }
          else if(rowFormat.compByteWidth == 2)
          {
            // 16-bit depth is normalised
            float f = (float)readObj<uint16_t>(data, end, ok);
            ret.push_back(f / (float)0x0000ffff);
          }
        }
        else if(rowFormat.compType == CompType::Double)
        {
          ret.push_back(readObj<double>(data, end, ok));
        }
        else
        {
          // unorm/snorm

          if(rowFormat.compByteWidth == 4)
          {
            // should never hit this - no 32bit unorm/snorm type
            qCritical() << "Unexpected 4-byte unorm/snorm value";
            ret.push_back((float)readObj<uint32_t>(data, end, ok) / (float)0xffffffff);
          }
          else if(rowFormat.compByteWidth == 2)
          {
            ret.push_back(interpret(rowFormat, readObj<uint16_t>(data, end, ok)));
          }
          else if(rowFormat.compByteWidth == 1)
          {
            ret.push_back(interpret(rowFormat, readObj<uint8_t>(data, end, ok)));
          }
        }
      }
    }

    if(rowFormat.BGRAOrder())
    {
      QVariant tmp = ret[2];
      ret[2] = ret[0];
      ret[0] = tmp;
    }
  }

  // we read off the end, return empty set
  if(!ok)
    ret.clear();

  return ret;
}

QString TypeString(const ShaderVariable &v)
{
  if(!v.members.isEmpty() || v.isStruct)
  {
    if(v.isStruct)
      return lit("struct");
    else
      return QFormatStr("%1[%2]").arg(TypeString(v.members[0])).arg(v.members.count());
  }

  QString typeStr = ToQStr(v.type);

  if(v.displayAsHex)
  {
    if(v.type == VarType::ULong)
      typeStr = lit("xlong");
    else if(v.type == VarType::UInt)
      typeStr = lit("xint");
    else if(v.type == VarType::UShort)
      typeStr = lit("xshort");
    else if(v.type == VarType::UByte)
      typeStr = lit("xbyte");
  }

  if(v.rows == 1 && v.columns == 1)
    return typeStr;
  if(v.rows == 1)
    return QFormatStr("%1%2").arg(typeStr).arg(v.columns);
  else
    return QFormatStr("%1%2x%3 (%4)")
        .arg(typeStr)
        .arg(v.rows)
        .arg(v.columns)
        .arg(v.rowMajor ? lit("row_major") : lit("column_major"));
}

template <typename el>
static QString RowValuesToString(int cols, bool hex, el x, el y, el z, el w)
{
  if(cols == 1)
    return Formatter::Format(x, hex);
  else if(cols == 2)
    return QFormatStr("%1, %2").arg(Formatter::Format(x, hex)).arg(Formatter::Format(y, hex));
  else if(cols == 3)
    return QFormatStr("%1, %2, %3")
        .arg(Formatter::Format(x, hex))
        .arg(Formatter::Format(y, hex))
        .arg(Formatter::Format(z, hex));
  else
    return QFormatStr("%1, %2, %3, %4")
        .arg(Formatter::Format(x, hex))
        .arg(Formatter::Format(y, hex))
        .arg(Formatter::Format(z, hex))
        .arg(Formatter::Format(w, hex));
}

QString RowString(const ShaderVariable &v, uint32_t row, VarType type)
{
  if(type == VarType::Unknown)
    type = v.type;

  if(type == VarType::Double)
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.dv[row * v.columns + 0],
                             v.value.dv[row * v.columns + 1], v.value.dv[row * v.columns + 2],
                             v.value.dv[row * v.columns + 3]);
  else if(type == VarType::SLong)
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.s64v[row * v.columns + 0],
                             v.value.s64v[row * v.columns + 1], v.value.s64v[row * v.columns + 2],
                             v.value.s64v[row * v.columns + 3]);
  else if(type == VarType::ULong)
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.u64v[row * v.columns + 0],
                             v.value.u64v[row * v.columns + 1], v.value.u64v[row * v.columns + 2],
                             v.value.u64v[row * v.columns + 3]);
  else if(type == VarType::SInt || type == VarType::SShort || type == VarType::SByte)
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.iv[row * v.columns + 0],
                             v.value.iv[row * v.columns + 1], v.value.iv[row * v.columns + 2],
                             v.value.iv[row * v.columns + 3]);
  else if(type == VarType::UInt || type == VarType::UShort || type == VarType::UByte)
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.uv[row * v.columns + 0],
                             v.value.uv[row * v.columns + 1], v.value.uv[row * v.columns + 2],
                             v.value.uv[row * v.columns + 3]);
  else
    return RowValuesToString((int)v.columns, v.displayAsHex, v.value.fv[row * v.columns + 0],
                             v.value.fv[row * v.columns + 1], v.value.fv[row * v.columns + 2],
                             v.value.fv[row * v.columns + 3]);
}

QString VarString(const ShaderVariable &v)
{
  if(!v.members.isEmpty())
    return QString();

  if(v.rows == 1)
    return RowString(v, 0);

  QString ret;
  for(int i = 0; i < (int)v.rows; i++)
  {
    if(i > 0)
      ret += lit("\n");
    ret += lit("{") + RowString(v, i) + lit("}");
  }

  return ret;
}

QString RowTypeString(const ShaderVariable &v)
{
  if(!v.members.isEmpty() || v.isStruct)
  {
    if(v.isStruct)
      return lit("struct");
    else
      return lit("flibbertygibbet");
  }

  if(v.rows == 0 && v.columns == 0)
    return lit("-");

  QString typeStr = ToQStr(v.type);

  if(v.displayAsHex)
  {
    if(v.type == VarType::ULong)
      typeStr = lit("xlong");
    else if(v.type == VarType::UInt)
      typeStr = lit("xint");
    else if(v.type == VarType::UShort)
      typeStr = lit("xshort");
    else if(v.type == VarType::UByte)
      typeStr = lit("xbyte");
  }

  if(v.columns == 1)
    return typeStr;

  return QFormatStr("%1%2").arg(typeStr).arg(v.columns);
}