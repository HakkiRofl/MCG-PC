#include <iostream>
#include <vector>
#include <bit>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_map>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
    using socklen_t = int;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Настройки
const int FRAME_WIDTH  = 640;  // Увеличено для лучшего качества
const int FRAME_HEIGHT = 480;  // Увеличено для лучшего качества
const int UDP_PORT = 12345;
const int MAX_UDP_PACKET_SIZE = 65535;
const int CHUNK_SIZE = 1400;  // должно совпадать с ESP32

// Глобальные переменные
std::vector<uint8_t> g_latestFrame;
std::mutex g_latestFrameMutex;
std::atomic<bool> g_newFrameAvailable(false);
int g_frameWidth = FRAME_WIDTH, g_frameHeight = FRAME_HEIGHT, g_frameChannels = 3;
// Метрики производительности
std::atomic<int> g_framesReceived(0);
std::atomic<int> g_framesDisplayed(0);
std::chrono::steady_clock::time_point g_lastFpsUpdate = std::chrono::steady_clock::now();
float g_currentFps = 0.0f;
float g_avgLatency = 0.0f;

// Сборщик кадров с оптимизацией
struct FrameAssembler {
    uint16_t total_fragments = 0;
    std::vector<uint8_t> jpeg_buffer;
    std::vector<uint8_t> fragment_received;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update;
    uint16_t received_count = 0;

    bool isComplete() const {
        return received_count == total_fragments;
    }
};

std::unordered_map<uint16_t, FrameAssembler> g_frameAssemblers;

std::mutex g_assemblerMutex;

// Функции чтения чисел из сетевого порядка байт
uint16_t readUint16(const uint8_t* buf) {
    uint16_t val;
    std::memcpy(&val, buf, sizeof(val));
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(val);
    else
        return val;
}

bool decodeJPEG(const uint8_t* jpeg_data, size_t jpeg_size,
                std::vector<uint8_t>& out_rgb, int& width, int& height, int& channels) {
    if (jpeg_size < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return false;
    }
    int w, h, c;
    unsigned char* img = stbi_load_from_memory(jpeg_data, jpeg_size, &w, &h, &c, 3);
    if (!img) {
        return false;
    }
    width = w;
    height = h;
    channels = 3;
    out_rgb.assign(img, img + (w * h * 3));
    stbi_image_free(img);
    return true;
}

void udpReceiverThread() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "❌ Socket creation failed\n";
        return;
    }

    // Увеличенный буфер приема для минимизации потерь
    int recvBufSize = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&recvBufSize, sizeof(recvBufSize));

    // Установка неблокирующего режима для улучшения производительности
#ifdef _WIN32
    u_long mode = 0; // 0 = blocking, 1 = non-blocking (пока оставим блокирующим)
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(UDP_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "❌ Bind failed on port " << UDP_PORT << std::endl;
        closesocket(sock);
        return;
    }

    std::cout << "🟢 Listening for UDP on port " << UDP_PORT << "..." << std::endl;

    char recvBuf[MAX_UDP_PACKET_SIZE];
    sockaddr_in clientAddr{};
    socklen_t clientAddrLen = sizeof(clientAddr);
    auto lastCleanup = std::chrono::steady_clock::now();

    while (true) {
        int bytesReceived = recvfrom(sock, recvBuf, sizeof(recvBuf), 0,
                                     (sockaddr*)&clientAddr, &clientAddrLen);
        if (bytesReceived > 8) {
            const uint8_t* header = reinterpret_cast<const uint8_t*>(recvBuf);
            uint16_t imgId      = readUint16(&header[0]);
            uint16_t fragId     = readUint16(&header[2]);
            uint16_t totalFrags = readUint16(&header[4]);
            uint16_t totalSize  = readUint16(&header[6]);

            const uint8_t* data = header + 8;
            size_t dataSize = bytesReceived - 8;

            // Валидация размера
            // if (totalSize == 0 || totalSize > 100 * 1024) {
            //     continue;
            // }

            // периодическая чистка
            auto now = std::chrono::steady_clock::now();

            if (now - lastCleanup > std::chrono::milliseconds(500)) {
                std::lock_guard lock(g_assemblerMutex);
                std::erase_if(g_frameAssemblers, [&](auto& item) {
                    return (now - item.second.last_update) > std::chrono::milliseconds(1000);
                });
                lastCleanup = now;
            }

            std::lock_guard<std::mutex> lock(g_assemblerMutex);
            // чтобы не создавать лишний объект
            auto [it, inserted] = g_frameAssemblers.try_emplace(imgId);
            FrameAssembler& assembler = it->second;
            if (inserted) {
                // инициализация
                assembler.total_fragments = 0;
                assembler.received_count = 0;
            }
            
            if (assembler.total_fragments == 0) {
                assembler.total_fragments = totalFrags;
                // Вроде как меньше аллокаций должно быть
                assembler.jpeg_buffer.resize(totalSize); // без переаллокации
                assembler.fragment_received.assign(totalFrags, false);
                assembler.start_time = now;
                assembler.received_count = 0;
            }
            
            assembler.last_update = now;

            if (fragId < totalFrags && !assembler.fragment_received[fragId]) {
                size_t offset = fragId * CHUNK_SIZE;
                if (offset + dataSize <= totalSize) {
                    std::memcpy(assembler.jpeg_buffer.data() + offset, data, dataSize);
                    assembler.fragment_received[fragId] = true;
                    assembler.received_count++;
                }
            }

            if (assembler.isComplete()) {
                std::vector<uint8_t> rgbData;
                int w, h, c;
                if (decodeJPEG(assembler.jpeg_buffer.data(), totalSize, rgbData, w, h, c)) {
                    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - assembler.start_time).count();
                    
                    // Обновление метрик
                    g_framesReceived++;
                    static float latency_sum = 0.0f;
                    static int latency_count = 0;
                    latency_sum += latency;
                    latency_count++;
                    g_avgLatency = latency_sum / latency_count;
                    
                    // Заменяем глобальный буфер новым кадром
                    {
                        std::lock_guard<std::mutex> frameLock(g_latestFrameMutex);
                        g_latestFrame.swap(rgbData);
                        g_frameWidth = w;
                        g_frameHeight = h;
                        g_frameChannels = c;
                        g_newFrameAvailable = true;
                    }
                }
                g_frameAssemblers.erase(imgId);
            }
        }
        
        // Убрал sleep для максимальной производительности
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
}

// --- OpenGL ---
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D ourTexture;
void main() {
    FragColor = texture(ourTexture, TexCoord);
}
)";

GLuint VAO, VBO, EBO, texture, shaderProgram;

void initOpenGL() {
    float vertices[] = {
        -1.0f,  1.0f,       0.0f, 1.0f,
        -1.0f, -1.0f,       0.0f, 0.0f,
         1.0f, -1.0f,       1.0f, 0.0f,
         1.0f,  1.0f,       1.0f, 1.0f
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FRAME_WIDTH, FRAME_HEIGHT, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        int success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char info[512];
            glGetShaderInfoLog(shader, 512, nullptr, info);
            std::cerr << "Shader error: " << info << std::endl;
        }
        return shader;
    };
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

int windowWidth = 800;
int windowHeight = 600;

void window_size_callback(GLFWwindow* window, int width, int height) {
    windowWidth = width;
    windowHeight = height;

    float windowAspect = static_cast<float>(windowWidth) / windowHeight;
    float logicalAspect = static_cast<float>(FRAME_WIDTH) / FRAME_HEIGHT;

    int viewportWidth, viewportHeight;
    int viewportX = 0, viewportY = 0;

    if (windowAspect > logicalAspect) {
        viewportHeight = windowHeight;
        viewportWidth = static_cast<int>(windowHeight * logicalAspect);
        viewportX = (windowWidth - viewportWidth) / 2;
    } else {
        viewportWidth = windowWidth;
        viewportHeight = static_cast<int>(windowWidth / logicalAspect);
        viewportY = (windowHeight - viewportHeight) / 2;
    }

    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
}

int main() {
    if (!glfwInit()) return -1;
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "ESP32-CAM Stream - FPS: 0.0", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // Отключаем VSync для максимального FPS

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
    
    glfwSetWindowSizeCallback(window, window_size_callback);
    window_size_callback(window, windowWidth, windowHeight);
    
    initOpenGL();

    std::thread udpThread(udpReceiverThread);
    udpThread.detach();

    auto lastFrameTime = std::chrono::steady_clock::now();
    int frameCount = 0;

    while (!glfwWindowShouldClose(window)) {
        auto currentTime = std::chrono::steady_clock::now();
        
        // Обновление FPS каждую секунду
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - g_lastFpsUpdate).count();
        if (elapsed >= 1000) {
            g_currentFps = g_framesDisplayed * 1000.0f / elapsed;
            
            // Обновление заголовка окна с метриками
            char title[256];
            snprintf(title, sizeof(title), 
                    "ESP32-CAM Stream - FPS: %.1f | Latency: %.0fms | Received: %d",
                    g_currentFps, g_avgLatency, g_framesReceived.load());
            glfwSetWindowTitle(window, title);
            
            g_framesDisplayed = 0;
            g_lastFpsUpdate = currentTime;
            
            // Вывод в консоль
            std::cout << "📊 FPS: " << g_currentFps 
                     << " | Latency: " << g_avgLatency << "ms"
                     << " | Total frames: " << g_framesReceived.load() << std::endl;
        }

        // при получении нового кадра
        if (g_newFrameAvailable) {
            std::lock_guard<std::mutex> lock(g_latestFrameMutex);
            if (!g_latestFrame.empty()) {
                glBindTexture(GL_TEXTURE_2D, texture);

                // Если размер изменился — перевыделяем текстуру
                static int lastWidth = 0, lastHeight = 0;
                if (g_frameWidth != lastWidth || g_frameHeight != lastHeight) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                 g_frameWidth, g_frameHeight, 0,
                                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                    lastWidth = g_frameWidth;
                    lastHeight = g_frameHeight;
                }

                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                g_frameWidth, g_frameHeight,
                                GL_RGB, GL_UNSIGNED_BYTE,
                                g_latestFrame.data());
                g_framesDisplayed++;
            }
            g_newFrameAvailable = false;
        }

        // Рендеринг
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(shaderProgram);
        glBindVertexArray(VAO);
        glBindTexture(GL_TEXTURE_2D, texture);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
