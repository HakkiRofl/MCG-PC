#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

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

// Настройки кадра (должны совпадать с ESP32)
const int FRAME_WIDTH  = 320;
const int FRAME_HEIGHT = 240;
const int BYTES_PER_PIXEL = 1;
const int FRAME_SIZE_BYTES = FRAME_WIDTH * FRAME_HEIGHT * BYTES_PER_PIXEL;
const int UDP_PORT = 12345;
const int MAX_UDP_PACKET_SIZE = 65535;

// Маркер начала кадра
const uint8_t FRAME_START_MARKER[4] = {0xAA, 0xBB, 0xCC, 0xDD};

// Глобальные переменные
std::vector<uint8_t> g_frameBuffer(FRAME_SIZE_BYTES);
std::atomic<bool> g_newFrameReady(false);
std::mutex g_frameMutex;

// Функция поиска маркера в буфере
size_t findMarker(const std::vector<uint8_t>& buffer, size_t startPos = 0) {
    if (buffer.size() < 4) return std::string::npos;
    for (size_t i = startPos; i <= buffer.size() - 4; ++i) {
        if (buffer[i] == FRAME_START_MARKER[0] &&
            buffer[i+1] == FRAME_START_MARKER[1] &&
            buffer[i+2] == FRAME_START_MARKER[2] &&
            buffer[i+3] == FRAME_START_MARKER[3]) {
            return i;
        }
    }
    return std::string::npos;
}

void udpReceiverThread() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        return;
    }

    // Увеличиваем буфер приёма для уменьшения потерь
    int recvBufSize = 1024 * 1024; // 1 МБ
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&recvBufSize, sizeof(recvBufSize));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(UDP_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed\n";
        closesocket(sock);
        return;
    }

    std::cout << "Listening on UDP port " << UDP_PORT << " with frame sync...\n";

    std::vector<uint8_t> accumulationBuffer;
    accumulationBuffer.reserve(FRAME_SIZE_BYTES * 4);
    char recvBuf[MAX_UDP_PACKET_SIZE];
    sockaddr_in clientAddr{};
    socklen_t clientAddrLen = sizeof(clientAddr);

    while (true) {
        int bytesReceived = recvfrom(sock, recvBuf, sizeof(recvBuf), 0,
                                     (sockaddr*)&clientAddr, &clientAddrLen);
        if (bytesReceived > 0) {
            // Добавляем данные в накопительный буфер
            accumulationBuffer.insert(accumulationBuffer.end(), recvBuf, recvBuf + bytesReceived);

            // Поиск маркера
            size_t markerPos = findMarker(accumulationBuffer);
            if (markerPos != std::string::npos) {
                // Удаляем всё, что было до маркера (мусор)
                if (markerPos > 0) {
                    accumulationBuffer.erase(accumulationBuffer.begin(), accumulationBuffer.begin() + markerPos);
                }
                // Проверяем, достаточно ли данных для полного кадра (маркер + данные)
                if (accumulationBuffer.size() >= 4 + FRAME_SIZE_BYTES) {
                    // Извлекаем данные кадра (сразу после маркера)
                    std::lock_guard<std::mutex> lock(g_frameMutex);
                    std::copy(accumulationBuffer.begin() + 4,
                              accumulationBuffer.begin() + 4 + FRAME_SIZE_BYTES,
                              g_frameBuffer.begin());
                    g_newFrameReady = true;
                    // Удаляем обработанный кадр из буфера
                    accumulationBuffer.erase(accumulationBuffer.begin(),
                                             accumulationBuffer.begin() + 4 + FRAME_SIZE_BYTES);
                }
                // Если данных недостаточно, ждём следующих пакетов
            }
        }
        // Небольшой sleep, чтобы не загружать CPU
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
}

// --- OpenGL часть (без изменений) ---
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
    float gray = texture(ourTexture, TexCoord).r;
    FragColor = vec4(gray, gray, gray, 1.0);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, FRAME_WIDTH, FRAME_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

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

int main() {
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(FRAME_WIDTH, FRAME_HEIGHT, "ESP32-CAM Sync Stream", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    initOpenGL();
    std::thread udpThread(udpReceiverThread);

    while (!glfwWindowShouldClose(window)) {
        if (g_newFrameReady) {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FRAME_WIDTH, FRAME_HEIGHT,
                            GL_RED, GL_UNSIGNED_BYTE, g_frameBuffer.data());
            g_newFrameReady = false;
        }
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