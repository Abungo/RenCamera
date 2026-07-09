#pragma once

#include <string>
#include <vector>
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <android/log.h>

#ifndef LOG_TAG
#define LOG_TAG "RenCamera/GL"
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

struct EglHeadlessSetup {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    bool init(std::string& errorLog) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            errorLog += "EGL: failed to get display\n";
            return false;
        }
        if (!eglInitialize(display, nullptr, nullptr)) {
            errorLog += "EGL: failed to initialize\n";
            return false;
        }

        EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        EGLConfig config;
        EGLint numConfigs;
        if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
            errorLog += "EGL: failed to choose config\n";
            return false;
        }

        EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
        };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
        if (context == EGL_NO_CONTEXT) {
            errorLog += "EGL: failed to create context (gl version 3 requested)\n";
            return false;
        }

        EGLint pbufferAttribs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
        if (surface == EGL_NO_SURFACE) {
            errorLog += "EGL: failed to create pbuffer surface\n";
            destroy();
            return false;
        }

        if (!eglMakeCurrent(display, surface, surface, context)) {
            errorLog += "EGL: failed to make current EGL context\n";
            destroy();
            return false;
        }
        return true;
    }

    void destroy() {
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) {
                eglDestroySurface(display, surface);
                surface = EGL_NO_SURFACE;
            }
            if (context != EGL_NO_CONTEXT) {
                eglDestroyContext(display, context);
                context = EGL_NO_CONTEXT;
            }
            eglTerminate(display);
            display = EGL_NO_DISPLAY;
        }
    }
    ~EglHeadlessSetup() {
        destroy();
    }
};

static inline void checkGlError(const char* op, std::string& errorLog) {
    for (GLint error = glGetError(); error; error = glGetError()) {
        errorLog += "after " + std::string(op) + "() glError (0x" + std::to_string(error) + ")\n";
        LOGE("after %s() glError (0x%x)", op, error);
    }
}

static inline GLuint compileShader(GLenum type, const char* source, std::string& errorLog) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        errorLog += "GL: failed to create shader object\n";
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 1) {
            std::vector<char> infoLog(infoLen);
            glGetShaderInfoLog(shader, infoLen, nullptr, infoLog.data());
            errorLog += "GLSL compilation error:\n" + std::string(infoLog.data()) + "\n";
            LOGE("GLSL compilation error:\n%s", infoLog.data());
        } else {
            errorLog += "GLSL compilation failed without details\n";
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static inline GLuint createComputeProgram(const char* source, std::string& errorLog) {
    GLuint shader = compileShader(GL_COMPUTE_SHADER, source, errorLog);
    if (shader == 0) return 0;
    GLuint program = glCreateProgram();
    if (program == 0) {
        errorLog += "GL: failed to create program object\n";
        glDeleteShader(shader);
        return 0;
    }
    glAttachShader(program, shader);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint infoLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 1) {
            std::vector<char> infoLog(infoLen);
            glGetProgramInfoLog(program, infoLen, nullptr, infoLog.data());
            errorLog += "GLSL program linking error:\n" + std::string(infoLog.data()) + "\n";
            LOGE("GLSL program linking error:\n%s", infoLog.data());
        } else {
            errorLog += "GLSL program linking failed without details\n";
        }
        glDeleteProgram(program);
        glDeleteShader(shader);
        return 0;
    }
    glDeleteShader(shader);
    return program;
}
