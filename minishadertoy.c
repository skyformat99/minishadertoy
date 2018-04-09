#define __STDC_WANT_LIB_EXT2__ 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <alloca.h>
#include <errno.h>
#include "glad.h"
#include "jfes/jfes.h"
#include <GLFW/glfw3.h>
#include "minishadertoy.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static GLFWwindow *_mainWindow;

static const char *shader_header = 
    "#version 300 es\n"
    "#extension GL_EXT_shader_texture_lod : enable\n"
    "#extension GL_OES_standard_derivatives : enable\n"
    "#define iGlobalTime iTime\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "out vec4 fragmentColor;\n"
    "uniform vec3      iResolution;\n"
    "uniform float     iTime;\n"
    "uniform float     iTimeDelta;\n"
    "uniform int       iFrame;\n"
    "uniform float     iChannelTime[4];\n"
    "uniform vec3      iChannelResolution[4];\n"
    "uniform vec4      iMouse;\n"
    "uniform vec4      iDate;\n"
    "uniform float     iSampleRate;\n"
    "uniform sampler2D iChannel0, iChannel1, iChannel2, iChannel3;\n";

static const char *shader_footer = 
    "\nvoid main(void) {\n"
    "    vec4 color = vec4(0.0,0.0,0.0,1.0);\n"
    "    mainImage(color, gl_FragCoord.xy);\n"
    "    color.w = 1.0;\n"
    "    fragmentColor = color;\n"
    "}\n";

#ifdef HAVE_CURL
#include <curl/curl.h>

struct buffer
{
    char *m_buffer;
    int m_buf_size;
};

static size_t buffer_write_cb(void *ptr, size_t size, size_t nmemb, void *stream)
{
    struct buffer *b = (struct buffer *)stream;
    int buf_pos = b->m_buf_size;
    b->m_buf_size += size*nmemb;
    b->m_buffer = realloc(b->m_buffer, b->m_buf_size);
    memcpy(b->m_buffer + buf_pos, ptr, size*nmemb);
    return size*nmemb;
}

static char *load_url(const char *url, int *size, int is_post)
{
    struct buffer b = { 0 };
    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();
    if (is_post)
    {
        char buf[256];
        char *id = strrchr(url, '/');
        if (!id)
            return 0;
        snprintf(buf, sizeof(buf), "s={ \"shaders\" : [\"%s\"] }", id + 1);
        curl_easy_setopt(curl, CURLOPT_URL, "https://www.shadertoy.com/shadertoy");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);
        curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.shadertoy.com/browse");
    } else
        curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        if (b.m_buffer)
            free(b.m_buffer);
        printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return 0;
    }
    curl_easy_cleanup(curl);
    *size = b.m_buf_size;
    //printf("%d readed\n", *size);
    return b.m_buffer;
}
#endif

static int mkpath(char *path)
{
    int len = (int)strlen(path);
    if (len <= 0)
        return 0;

    char *buffer = (char*)malloc(len + 1);
    if (!buffer)
        goto fail;
    strcpy(buffer, path);

    if (buffer[len - 1] == '/')
        buffer[len - 1] = '\0';
    if (mkdir(buffer, 0777) == 0)
    {
        free(buffer);
        return 1;
    }

    char *p = buffer + 1;
    while (1)
    {
        while (*p && *p != '\\' && *p != '/')
            p++;
        if (!*p)
            break;
        char sav = *p;
        *p = 0;
        if ((mkdir(buffer, 0777) == -1) && (errno == ENOENT))
            goto fail;
        *p++ = sav;
    }
    free(buffer);
    return 1;
fail:
    if (buffer)
        free(buffer);
    return 0;
}

static unsigned char *load_file(const char *fname, int *data_size)
{
    FILE *file = fopen(fname, "rb");
    unsigned char *data;
    *data_size = 0;
    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    *data_size = (int)ftell(file);
    fseek(file, 0, SEEK_SET);
    data = (unsigned char*)malloc(*data_size);
    if (!data)
        goto fail;
    if ((int)fread(data, 1, *data_size, file) != *data_size)
        exit(1);
fail:
    fclose(file);
    return data;
}

static int codepoint_utf8(uint32_t codepoint, char **buf)
{
    int len = 0;
    char *p = *buf;
    if (codepoint >= 0x110000u)
        return 0;
    if (codepoint < 0x80u)
    {
        p[len++] = (char)codepoint;
    } else if (codepoint < 0x800u)
    {
        p[len++] = (char)(0xC0 | codepoint >> 6 & 0x1F);
        p[len++] = (char)(0x80 | codepoint & 0x3F);
    } else if (codepoint < 0x10000u)
    {
        p[len++] = (char)(0xE0 | codepoint >> 12 & 0xF);
        p[len++] = (char)(0x80 | codepoint >> 6 & 0x3F);
        p[len++] = (char)(0x80 | codepoint & 0x3F);
    } else
    {
        p[len++] = (char)(0xF0 | codepoint >> 18 & 0x7);
        p[len++] = (char)(0x80 | codepoint >> 12 & 0x3F);
        p[len++] = (char)(0x80 | codepoint >> 6 & 0x3F);
        p[len++] = (char)(0x80 | codepoint & 0x3F);
    }
    return len;
}

static int unescape_json(char *buf, int buf_len, char *out_buf)
{
    enum {
        JSON_INITIAL,
        JSON_ESCAPE,
        JSON_UNICODE,
        JSON_UTF16_I1,
        JSON_UTF16_I2,
        JSON_UTF16_LS
    };
    int i, ubuf_offset, len = 0, state = JSON_INITIAL;
    uint32_t u1, codepoint;
    char ch, *p = out_buf, *pt;
    char ubuf[5];
    ubuf[4] = 0;
    for (i = 0; i < buf_len; i++)
    {
        ch = buf[i];
        if (state == JSON_INITIAL)
        {
            if (ch != '\\')
                p[len++] = ch;
            else
                state = JSON_ESCAPE;
        } else if (state == JSON_ESCAPE || state == JSON_UTF16_I2)
        {
            int old_state = state;
            state = JSON_INITIAL;
            switch(ch)
            {
                case 'n': p[len++] = '\n'; break;
                case 't': p[len++] = '\t'; break;
                case 'r': p[len++] = '\r'; break;
                case 'b': p[len++] = '\b'; break;
                case 'f': p[len++] = '\f'; break;
                case 'u': state = (JSON_ESCAPE == old_state) ? JSON_UNICODE : JSON_UTF16_LS; ubuf_offset = 0; break;
                default: p[len++] = ch; break;
            }
        } else if (state == JSON_UNICODE)
        {
            ubuf[ubuf_offset++] = ch;
            if (ubuf_offset == 4)
            {
                codepoint = strtol(ubuf, NULL, 16);
                if ((codepoint & 0xFC00u) == 0xD800u)
                {
                    u1 = codepoint;
                    state = JSON_UTF16_I1;
                } else
                {
                    pt = &p[len];
                    len += codepoint_utf8(codepoint, &pt);
                    state = JSON_INITIAL;
                }
            }
        } else if (state == JSON_UTF16_I1)
        {
            if (ch != '\\')
            {
                p[len++] = ch;
                state = JSON_INITIAL;
            } else
                state = JSON_UTF16_I2;
        } else if (state == JSON_UTF16_LS)
        {
            ubuf[ubuf_offset++] = ch;
            if (ubuf_offset == 4)
            {
                codepoint = strtol(ubuf, NULL, 16);
                if ((codepoint & 0xFC00u) == 0xDC00u)
                    codepoint = ((u1 & 0x3FFu) << 10 | codepoint & 0x3FFu) + 0x10000u;
                pt = &p[len];
                len += codepoint_utf8(codepoint, &pt);
                state = JSON_INITIAL;
            }
        }
    }
    p[len] = 0;
    return len;
}

void CheckGLErrors(const char *func, int line)
{
    int lasterror = glGetError();
    if (lasterror)
    {
        printf("OpenGL error in %s, %i: err=%i\n", func, line, lasterror); fflush(stdout);
    }
}

static void gl_init()
{
    if (!glfwInit())
    {
        printf("error: glfw init failed\n");
        exit(1);
    }
#ifdef USE_GLES3
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_RESIZABLE, 1);
    _mainWindow = glfwCreateWindow(600, 400, "Shadertoy", NULL, NULL);
    if (!_mainWindow)
    {
        printf("error: create window failed\n"); fflush(stdout);
        exit(1);
    }
    glfwMakeContextCurrent(_mainWindow);
#ifdef USE_GLES3
    glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)glfwGetProcAddress("glGenerateMipmap");
#endif
    glfwSetInputMode(_mainWindow, GLFW_STICKY_MOUSE_BUTTONS, 1);

    gladLoadGL();
}

static void gl_close()
{
    glfwDestroyWindow(_mainWindow);
    glfwTerminate();
}

static int load_image(const stbi_uc *data, int len, SAMPLER *s)
{
    GLuint tex;
    int width, height, n;
    stbi_set_flip_vertically_on_load(s ? s->vflip : 0);
    unsigned char *pix = stbi_load_from_memory(data, len, &width, &height, &n, 4);
    if (!pix)
        return 0;
    glGenTextures(1, &tex); GLCHK;
    glBindTexture(GL_TEXTURE_2D, tex); GLCHK;
#ifndef USE_GLES3
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE); GLCHK;
#endif
    int clamp = GL_CLAMP_TO_EDGE, min_filter = GL_LINEAR_MIPMAP_LINEAR, mag_filter = GL_LINEAR;
    if (s)
    {
        clamp = s->wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        if (!s->filter)
        {
            min_filter = GL_NEAREST_MIPMAP_NEAREST;
            mag_filter = GL_NEAREST;
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp); GLCHK;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix); GLCHK;
#ifdef USE_GLES3
    glGenerateMipmap(GL_TEXTURE_2D); GLCHK;
#endif
    glBindTexture(GL_TEXTURE_2D, 0); GLCHK;
    return (int)tex;
}

void fb_delete(FBO *f)
{
    if (f->framebuffer)
        glDeleteFramebuffers(1, &f->framebuffer); GLCHK;
    if (f->framebufferTex)
        glDeleteTextures(1, &f->framebufferTex); GLCHK;
}

void fb_init(FBO *f, int width, int height, int float_tex)
{
    f->floatTex = float_tex;
    glGenFramebuffers(1, &f->framebuffer); GLCHK;
    glBindFramebuffer(GL_FRAMEBUFFER, f->framebuffer); GLCHK;
    glGenTextures(1, &f->framebufferTex); GLCHK;
    glBindTexture(GL_TEXTURE_2D, f->framebufferTex); GLCHK;
    if (f->floatTex)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, 0);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); GLCHK;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); GLCHK;
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, f->framebufferTex, 0); GLCHK;
    glBindTexture(GL_TEXTURE_2D, 0); GLCHK;
    glBindFramebuffer(GL_FRAMEBUFFER, 0); GLCHK;
}

void shader_delete(SHADER *s)
{
    if (s->shader)
        glDeleteShader(s->shader);
    if (s->prog)
        glDeleteProgram(s->prog);
}

int shader_init(SHADER *s, const char *pCode, int is_compute)
{
    size_t hdr_len = strlen(shader_header);
    size_t source_len = strlen(pCode);
    size_t footer_len = strlen(shader_footer);
    GLchar *sh = (GLchar *)malloc(hdr_len + source_len + footer_len + 1);
    memcpy(sh, shader_header, hdr_len);
    memcpy(sh + hdr_len, pCode, source_len);
    sh[hdr_len + source_len] = 0;
    if (!strstr(sh, "void main("))
    {
        memcpy(sh + hdr_len + source_len, shader_footer, footer_len);
        sh[hdr_len + source_len + footer_len] = 0;
    }

    s->prog = glCreateProgram(); GLCHK;
    s->shader = glCreateShader(is_compute ? GL_COMPUTE_SHADER : GL_FRAGMENT_SHADER); GLCHK;
    glShaderSource(s->shader, 1, (const GLchar **)&sh, 0); GLCHK;
    glCompileShader(s->shader); GLCHK;
#ifdef _DEBUG
    GLint isCompiled = 0;
    glGetShaderiv(s->shader, GL_COMPILE_STATUS, &isCompiled);
    if (isCompiled == GL_FALSE)
    {
        GLint maxLength = 0;
        glGetShaderiv(s->shader, GL_INFO_LOG_LENGTH, &maxLength);
        GLchar *errorLog = (GLchar *)alloca(maxLength);
        glGetShaderInfoLog(s->shader, maxLength, &maxLength, &errorLog[0]);
        printf("compile error: %s", errorLog);
        printf("code: %s", sh);
        exit(1);
    }
#endif
    free(sh);
    glAttachShader(s->prog, s->shader); GLCHK;
    glLinkProgram(s->prog); GLCHK;
#ifdef _DEBUG
    GLint isLinked = 0;
    glGetProgramiv(s->prog, GL_LINK_STATUS, &isLinked); GLCHK;
    if (isLinked == GL_FALSE)
    {
        GLint maxLength = 0;
        glGetProgramiv(s->prog, GL_INFO_LOG_LENGTH, &maxLength); GLCHK;
        GLchar *errorLog = (GLchar *)alloca(maxLength);
        glGetProgramInfoLog(s->prog, maxLength, &maxLength, &errorLog[0]); GLCHK;
        printf("link error: %s", errorLog);
        exit(1);
    }
#endif
    s->iResolution = glGetUniformLocation(s->prog, "iResolution"); GLCHK;
    s->iTime       = glGetUniformLocation(s->prog, "iTime"); GLCHK;
    s->iTimeDelta  = glGetUniformLocation(s->prog, "iTimeDelta"); GLCHK;
    s->iFrame      = glGetUniformLocation(s->prog, "iFrame"); GLCHK;
    s->iMouse      = glGetUniformLocation(s->prog, "iMouse"); GLCHK;
    s->iDate       = glGetUniformLocation(s->prog, "iDate"); GLCHK;
    s->iSampleRate = glGetUniformLocation(s->prog, "iSampleRate"); GLCHK;
    for (int i = 0; i < 4; i++) 
    {
        char buf[64];
        sprintf(buf, "iChannel%d", i);
        s->iChannel[i] = glGetUniformLocation(s->prog, buf); GLCHK;
        sprintf(buf, "iChannelTime[%d]", i);
        s->iChannelTime[i] = glGetUniformLocation(s->prog, buf); GLCHK;
        sprintf(buf, "iChannelResolution[%d]", i);
        s->iChannelResolution[i] = glGetUniformLocation(s->prog, buf); GLCHK;
    }
    return 1;
}

static int switch_val(jfes_value_t *str, const char **vals)
{
    for (int i = 0; *vals; i++, vals++)
        if (!strcmp(*vals, str->data.string_val.data))
            return i;
    perror(str->data.string_val.data);
    assert(0);
    return -1;
}

int main(int argc, char **argv)
{
    int buf_size;
    char *buffer;
    if (argc < 2)
    {
        printf("usage: toy url or file\n");
        return 0;
    }
#ifdef HAVE_CURL
    if (strstr(argv[1], "://"))
        buffer = load_url(argv[1], &buf_size, 1);
    else
#endif
        buffer = load_file(argv[1], &buf_size);
    if (!buffer)
        return 1;

    gl_init();

    jfes_config_t config;
    config.jfes_malloc = (jfes_malloc_t)malloc;
    config.jfes_free = free;

    jfes_value_t value;
    jfes_status_t status = jfes_parse_to_value(&config, buffer, buf_size, &value);
    if (!jfes_status_is_good(status))
       return 1;
    jfes_value_t *root = value.data.array_val->items[0];
    jfes_value_t *rp = jfes_get_child(root, "renderpass", 0);

    SHADER shaders[5];
    memset(shaders, 0, sizeof(shaders));

    for (int i = 0; i < rp->data.array_val->count; i++)
    {
        SHADER *s = &shaders[i];
        jfes_value_t *pass = rp->data.array_val->items[i];
        jfes_value_t *inputs  = jfes_get_child(pass, "inputs", 0);
        jfes_value_t *outputs = jfes_get_child(pass, "outputs", 0);
        jfes_value_t *code    = jfes_get_child(pass, "code", 0);
        jfes_value_t *type    = jfes_get_child(pass, "type", 0);
        int j;
        for (j = 0; j < inputs->data.array_val->count; j++)
        {
           static const char *types[] = { "texture", "buffer", "cubemap", "musicstream", "keyboard", 0 };
           jfes_value_t *input = inputs->data.array_val->items[j];
           jfes_value_t *iid   = jfes_get_child(input, "id", 0);
           int itype = switch_val(jfes_get_child(input, "type", 0), types);
           jfes_value_t *ichannel = jfes_get_child(input, "channel", 0);
           jfes_value_t *filepath = jfes_get_child(input, "filepath", 0);
           jfes_value_t *sampler  = jfes_get_child(input, "sampler", 0);
           SHADER_INPUT *inp = s->inputs + ichannel->data.int_val;
           SAMPLER *smp = &inp->sampler;
           inp->id = iid->data.string_val.data;
           if (sampler)
           {
              static const char *filter[] = { "nearest", "linear", "mipmap", 0 };
              static const char *wrap[]   = { "clamp", "repeat", 0 };
              static const char *bools[]  = { "false", "true", 0 };
              static const char *internal[] = { "byte", 0 };
              smp->filter = switch_val(jfes_get_child(sampler, "filter", 0), filter);
              smp->wrap   = switch_val(jfes_get_child(sampler, "wrap", 0), wrap);
              smp->vflip  = switch_val(jfes_get_child(sampler, "vflip", 0), bools);
              smp->srgb   = switch_val(jfes_get_child(sampler, "srgb", 0), bools);
              smp->internal = switch_val(jfes_get_child(sampler, "internal", 0), internal);
           }
           if (filepath && 0 == itype)
           {
                char *buf = malloc(filepath->data.string_val.size + 26);
                strcpy(buf, "https://www.shadertoy.com");
                unescape_json(filepath->data.string_val.data, filepath->data.string_val.size, buf + 25);
                char *img = load_file(buf + 26, &buf_size);
#ifdef HAVE_CURL
                if (!img)
                {
                    img = load_url(buf, &buf_size, 0);
                    printf("load %s (%d bytes)\n", buf, buf_size);
                    mkpath(filepath->data.string_val.data + 1);
                    FILE *f = fopen(filepath->data.string_val.data + 1, "wb");
                    if (f)
                    {
                        fwrite(img, 1, buf_size, f);
                        fclose(f);
                    }
                }
#endif
                free(buf);
                if (img)
                {
                    inp->tex = load_image(img, buf_size, smp);
                }
           }
           //printf("i type=%d, id=%s, channel=%d\n", itype, inp->id, ichannel->data.int_val);
        }
        for (j = 0; j < outputs->data.array_val->count; j++)
        {
           jfes_value_t *output = outputs->data.array_val->items[j];
           jfes_value_t *oid    = jfes_get_child(output, "id", 0);
           jfes_value_t *ochannel = jfes_get_child(output, "channel", 0);
           s->output.id = oid->data.string_val.data;
           //printf("o id=%s, channel=%d\n", s->output.id, ochannel->data.int_val);
        }
        //printf("type=%s\n", type->data.string_val.data);
        char *unesc_buf = strdup(code->data.string_val.data);
        unescape_json(code->data.string_val.data, code->data.string_val.size, unesc_buf);
        shader_init(s, unesc_buf, 0);
        free(unesc_buf);
    }
    int frame = 0;
    double time_start = glfwGetTime(), time_last = time_start;
    while (!glfwWindowShouldClose(_mainWindow))
    {
        glfwPollEvents();
        int winWidth, winHeight, width, height, mkeys = 0;
        double mx, my;
        glfwGetWindowSize(_mainWindow, &winWidth, &winHeight);
        glfwGetFramebufferSize(_mainWindow, &width, &height);
        glfwGetCursorPos(_mainWindow, &mx, &my);
        float cx = -1.0f, cy = -1.0f;
        if (GLFW_PRESS == glfwGetMouseButton(_mainWindow, GLFW_MOUSE_BUTTON_LEFT))
        {
            cx = mx, cy = my;
        }
        glViewport(0, 0, winWidth, winHeight); GLCHK;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); GLCHK;

        SHADER *s = &shaders[0];
        float cur_time = glfwGetTime() - time_start;
        time_t rawtime;
        time(&rawtime);
        struct tm *tm = localtime(&rawtime);

        glUseProgram(s->prog); GLCHK;
        glUniform3f(s->iResolution, (float)winWidth, (float)winHeight, 1.0f); GLCHK;
        glUniform1f(s->iTime, cur_time); GLCHK;
        glUniform1f(s->iTimeDelta, cur_time - time_last); GLCHK;
        glUniform1i(s->iFrame, frame++); GLCHK;
	if(cx > -0.5f)
            glUniform4f(s->iMouse, mx, my, cx, cy); GLCHK;
        glUniform4f(s->iDate, tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour*3600 + tm->tm_min*60 + tm->tm_sec); GLCHK;
        glUniform1f(s->iSampleRate, 0); GLCHK;

        glActiveTexture(GL_TEXTURE0); GLCHK;
        glBindTexture(GL_TEXTURE_2D, 0); GLCHK;
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f); GLCHK;
        for (int i = 0, tu = 1; i < 4; i++)
        {
            glUniform1f(s->iChannelTime[i], cur_time); GLCHK;
            int w = 0, h = 0;
            if (s->inputs[i].tex)
            {
                glActiveTexture(GL_TEXTURE0 + tu); GLCHK;
                glBindTexture(GL_TEXTURE_2D, s->inputs[i].tex); GLCHK;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w); GLCHK;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h); GLCHK;
                glUniform1i(s->iChannel[i], tu); GLCHK;
                tu++;
            } else
                glUniform1i(s->iChannel[i], 0); GLCHK;
            glUniform3f(s->iChannelResolution[i], w, h, 1.0f); GLCHK;
        }

        glRecti(1, 1, -1, -1); GLCHK;
        glUseProgram(0); GLCHK;
        time_last = cur_time;
        glfwSwapBuffers(_mainWindow);
    }

    jfes_free_value(&config, &value);
    if (buffer)
        free(buffer);
    return 0;
}