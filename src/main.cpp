#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <map>
#include <algorithm>

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
const int FRAME_WIDTH  = 480;
const int FRAME_HEIGHT = 320;
const int UDP_PORT = 12345;
const int MAX_UDP_PACKET_SIZE = 65535;
const int CHUNK_SIZE = 1400;  // должно совпадать с ESP32

// Глобальные переменные
std::vector<uint8_t> g_latestFrame;          // Содержит всегда последний RGB-кадр
std::mutex g_latestFrameMutex;               // Мьютекс для доступа к g_latestFrame
std::atomic<bool> g_newFrameAvailable(false); // Флаг: есть новый кадр для отображения
int g_frameWidth = FRAME_WIDTH, g_frameHeight = FRAME_HEIGHT, g_frameChannels = 3;

// Сборщик кадров
struct FrameAssembler {
    uint16_t total_fragments = 0;
    std::vector<uint8_t> jpeg_buffer;
    std::vector<bool> fragment_received;
    std::chrono::steady_clock::time_point last_update;

    bool isComplete() {
        for (bool received : fragment_received) {
            if (!received) return false;
        }
        return true;
    }
};

std::map<uint16_t, FrameAssembler> g_frameAssemblers;

// Функции чтения чисел из сетевого порядка байт
uint16_t readUint16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

bool decodeJPEG(const uint8_t* jpeg_data, size_t jpeg_size,
                std::vector<uint8_t>& out_rgb, int& width, int& height, int& channels) {
    if (jpeg_size < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        std::cerr << "❌ Not a JPEG (missing SOI marker)" << std::endl;
        return false;
    }
    int w, h, c;
    unsigned char* img = stbi_load_from_memory(jpeg_data, jpeg_size, &w, &h, &c, 3);
    if (!img) {
        std::cerr << "❌ stb_image failed: " << stbi_failure_reason() << std::endl;
        return false;
    }
    width = w;
    height = h;
    channels = 3;
    out_rgb.assign(img, img + (w * h * 3));
    stbi_image_free(img);
    std::cout << "✅ JPEG decoded: " << w << "x" << h << std::endl;
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

    int recvBufSize = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&recvBufSize, sizeof(recvBufSize));

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
    int packetCount = 0;

    while (true) {
        int bytesReceived = recvfrom(sock, recvBuf, sizeof(recvBuf), 0,
                                     (sockaddr*)&clientAddr, &clientAddrLen);
        if (bytesReceived > 0) {
            packetCount++;
            if (packetCount % 100 == 0) {
                std::cout << "📦 Received " << packetCount << " packets" << std::endl;
            }

            // Заголовок теперь всегда 8 байт
            if (bytesReceived <= 8) {
                std::cerr << "⚠️ Packet too small: " << bytesReceived << std::endl;
                continue;
            }

            const uint8_t* header = reinterpret_cast<const uint8_t*>(recvBuf);
            uint16_t imgId      = readUint16(&header[0]);
            uint16_t fragId     = readUint16(&header[2]);
            uint16_t totalFrags = readUint16(&header[4]);
            uint16_t totalSize  = readUint16(&header[6]);  // до 65535 байт

            const uint8_t* data = header + 8;
            size_t dataSize = bytesReceived - 8;

            // Проверка на разумный размер (JPEG обычно не более 64 КБ)
            if (totalSize == 0 || totalSize > 64 * 1024) {
                std::cerr << "⚠️ Suspicious totalSize=" << totalSize << ", ignoring" << std::endl;
                continue;
            }

            static uint16_t lastImgId = 0xFFFF;
            if (imgId != lastImgId) {
                std::cout << "🆕 New frame ID=" << imgId
                          << ", fragments=" << totalFrags
                          << ", total size=" << totalSize << std::endl;
                lastImgId = imgId;
            }

            // Очистка старых сборщиков
            auto now = std::chrono::steady_clock::now();
            for (auto it = g_frameAssemblers.begin(); it != g_frameAssemblers.end(); ) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_update).count() > 2000) {
                    std::cout << "🧹 Discarding stale frame ID=" << it->first << std::endl;
                    it = g_frameAssemblers.erase(it);
                } else {
                    ++it;
                }
            }

            FrameAssembler& assembler = g_frameAssemblers[imgId];
            assembler.last_update = now;

            if (assembler.total_fragments == 0) {
                assembler.total_fragments = totalFrags;
                assembler.jpeg_buffer.resize(totalSize);
                assembler.fragment_received.assign(totalFrags, false);
            }

            if (fragId < totalFrags && !assembler.fragment_received[fragId]) {
                size_t offset = fragId * CHUNK_SIZE;
                if (offset + dataSize <= totalSize) {
                    std::memcpy(assembler.jpeg_buffer.data() + offset, data, dataSize);
                    assembler.fragment_received[fragId] = true;
                } else {
                    std::cerr << "⚠️ Fragment out of bounds: offset=" << offset
                              << ", dataSize=" << dataSize << ", total=" << totalSize << std::endl;
                }
            }

            if (assembler.isComplete()) {
                std::vector<uint8_t> rgbData;
                int w, h, c;
                if (decodeJPEG(assembler.jpeg_buffer.data(), totalSize, rgbData, w, h, c)) {
                    // Заменяем глобальный буфер новым кадром (пропускаем старые)
                    {
                        std::lock_guard<std::mutex> lock(g_latestFrameMutex);
                        g_latestFrame.swap(rgbData);  // эффективный обмен без копирования
                        g_frameWidth = w;
                        g_frameHeight = h;
                        g_frameChannels = c;
                        g_newFrameAvailable = true;    // сообщаем потоку рендеринга
                    }
                }
                g_frameAssemblers.erase(imgId);
            }
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
}

// --- OpenGL (без изменений, но для краткости опущен, используйте предыдущий код) ---
// ... (initOpenGL, main loop)
// --- OpenGL (без изменений, но добавлен вывод при обновлении текстуры) ---
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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





// Текущий размер окна
int windowWidth = 800;
int windowHeight = 600;

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    // Этот колбэк вызывается, когда меняется размер фреймбуфера.
    // Мы игнорируем его, так как хотим управлять viewport вручную.
}

void window_size_callback(GLFWwindow* window, int width, int height) {
    // Этот колбэк вызывается при изменении размера окна.
    windowWidth = width;
    windowHeight = height;

    // Рассчитываем соотношение сторон окна и логического разрешения
    float windowAspect = static_cast<float>(windowWidth) / windowHeight;
    float logicalAspect = static_cast<float>(FRAME_WIDTH) / FRAME_HEIGHT;

    int viewportWidth, viewportHeight;
    int viewportX = 0, viewportY = 0;

    // Выбираем стратегию масштабирования "целое число" или "максимальное заполнение"
    // Здесь пример "максимальное заполнение" с черными полосами (letterboxing)
    if (windowAspect > logicalAspect) {
        // Окно шире, чем логическое разрешение. Высота будет на всю высоту, ширина меньше.
        viewportHeight = windowHeight;
        viewportWidth = static_cast<int>(windowHeight * logicalAspect);
        viewportX = (windowWidth - viewportWidth) / 2; // Центрируем по горизонтали
    } else {
        // Окно уже, чем логическое разрешение. Ширина будет на всю ширину, высота меньше.
        viewportWidth = windowWidth;
        viewportHeight = static_cast<int>(windowWidth / logicalAspect);
        viewportY = (windowHeight - viewportHeight) / 2; // Центрируем по вертикали
    }

    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    // Здесь нужно пересчитать матрицу проекции (например, ортографическую)
    // Она должна отображать логическое разрешение (LOGICAL_WIDTH x LOGICAL_HEIGHT)
    // на область, определенную viewport.
    // Пример для ортографической проекции (используя библиотеку glm или аналог):
    // projection = glm::ortho(0.0f, static_cast<float>(LOGICAL_WIDTH), static_cast<float>(LOGICAL_HEIGHT), 0.0f);
    // Загрузите эту матрицу в ваш шейдер.
    std::cout << "Viewport: (" << viewportX << ", " << viewportY << ") " << viewportWidth << "x" << viewportHeight << std::endl;
}

int main() {
    if (!glfwInit()) return -1;
    // Разрешаем изменение размера
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Ключевая настройка: отключаем автоматическое масштабирование фреймбуфера
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(FRAME_WIDTH, FRAME_HEIGHT, "ESP32-CAM Stream", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    // Вертикальная синхронизация
    glfwSwapInterval(0);


    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
    // Устанавливаем колбэки
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback); // Можно не устанавливать, если не нужно
    glfwSetWindowSizeCallback(window, window_size_callback);

    // Инициализируем viewport и проекцию один раз при старте
    window_size_callback(window, windowWidth, windowHeight);
    initOpenGL();

    std::thread udpThread(udpReceiverThread);

    int displayCount = 0;
    while (!glfwWindowShouldClose(window)) {

        // Проверяем, есть ли новый кадр
        if (g_newFrameAvailable) {
            std::lock_guard<std::mutex> lock(g_latestFrameMutex);
            if (!g_latestFrame.empty()) {
                glBindTexture(GL_TEXTURE_2D, texture);
                // Используем glTexSubImage2D для быстрого обновления
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                g_frameWidth, g_frameHeight,
                                GL_RGB, GL_UNSIGNED_BYTE,
                                g_latestFrame.data());
            }
            g_newFrameAvailable = false; // сбрасываем флаг
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
    udpThread.detach();
    return 0;
}