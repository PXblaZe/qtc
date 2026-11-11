// Qt Shader Tools for qtc_rhi_shader_bake: GLSL compiled for every graphics backend. A library
// of its own (glslang and SPIRV-Cross are most of a megabyte of code), loaded the first time a
// program bakes a shader.
#include "../qtc_module.h"

#include <rhi/qshader.h>
#include <rhi/qshaderbaker.h>

namespace qtc {
namespace {

QByteArray bake(int stage, const QByteArray &src, QString *error) {
    QShader::Stage st = stage == QTC_RHI_VERTEX     ? QShader::VertexStage
                        : stage == QTC_RHI_FRAGMENT ? QShader::FragmentStage
                                                    : QShader::ComputeStage;
    // every backend of this platform: SPIR-V for Vulkan, GLSL for OpenGL (ES), and HLSL or MSL
    // where Direct3D or Metal exist; the legacy GLSL versions go when the shader needs more
    QList<QShaderBaker::GeneratedShader> extra;
#ifdef Q_OS_WIN
    extra.append({QShader::HlslShader, QShaderVersion(50)});
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    extra.append({QShader::MslShader, QShaderVersion(12)});
#endif
    QList<QList<QShaderBaker::GeneratedShader>> attempts;
    if (st == QShader::ComputeStage) {
        attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(310, QShaderVersion::GlslEs)},
                         {QShader::GlslShader, QShaderVersion(430)}});
        attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(430)}});
    } else {
        attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(100, QShaderVersion::GlslEs)},
                         {QShader::GlslShader, QShaderVersion(120)}, {QShader::GlslShader, QShaderVersion(150)},
                         {QShader::GlslShader, QShaderVersion(330)}});
        attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(300, QShaderVersion::GlslEs)},
                         {QShader::GlslShader, QShaderVersion(330)}});
    }
    QString first_error;
    for (QList<QShaderBaker::GeneratedShader> targets : attempts) {
        targets += extra;
        QShaderBaker b;
        b.setSourceString(src, st);
        b.setGeneratedShaders(targets);
        b.setGeneratedShaderVariants({QShader::StandardShader});
        const QShader s = b.bake();
        if (s.isValid()) return s.serialized();
        if (first_error.isEmpty()) first_error = b.errorMessage();
    }
    *error = first_error;
    return QByteArray();
}

}  // namespace
}  // namespace qtc

QTC_MODULE_INIT(ShaderTools) { qtc::g_module_hooks.bake_shader = qtc::bake; }
