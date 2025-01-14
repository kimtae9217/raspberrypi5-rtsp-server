/**
 * @file Global.cpp
 * @brief Global 헤더의 구현부
 * @details Global 클래스의 멤버 함수를 구현한 소스 파일
 * 
 * Copyright (c) 2024 rtspMediaStream
 * This project is licensed under the MIT License - see the LICENSE file for details
 */

#include "Global.h"

#include <chrono>
#include <random>
#include <netdb.h>
#include <iomanip>
#include <unistd.h>
#include <arpa/inet.h>

/**
 * @details 이 함수는 현재 시간을 NTP(Network Time Protocol)타임스탬프 형식으로 반환
 */
uint64_t GetTime() {
    auto now = std::chrono::system_clock::now();
    auto msSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    uint32_t ntpSeconds = (msSinceEpoch / 1000) + 2208988800U; // 1970 to 1900
    uint32_t ntpFraction = (msSinceEpoch % 1000) * 4294967;
    uint64_t ntpTime = ((uint64_t)ntpSeconds << 32) | ntpFraction;
    return ntpTime;
}

/**
 * @details 이 함수는 입력으로 받은 비트 수(n)에 따라 16비트 또는 32비트 범위의 랜덤 숫자를 생성하여 반환
 */
uint32_t GetRanNum(int n) {
    std::random_device rd; // make random seed
    std::mt19937 generator(rd()); // init random number generator
    if (n == 32) {
        // generate random number between 0 and 2^32-1
        std::uniform_int_distribution<uint32_t> distribution(1, 0xFFFFFFFF);
        return distribution(generator);
    } else if (n == 16) {
        std::uniform_int_distribution<uint32_t> distribution(1, 65535);
        return distribution(generator);
    }
    return 0;
}

/**
 * @details 이 함수는 시스템의 호스트 이름을 기반으로 서버의 IP 주소를 조회하여 반환
 */
std::string GetServerIP() {
    char hostbuffer[256];
    char *IPbuffer;
    struct hostent *host_entry;
    int hostname;

    // Get hostname
    hostname = gethostname(hostbuffer, sizeof(hostbuffer));
    if (hostname == -1) {
        perror("gethostname");
        return std::string("");
    }

    // Get host information
    host_entry = gethostbyname(hostbuffer);
    if (host_entry == nullptr) {
        perror("gethostbyname");
        return std::string("");
    }

    // Convert host's binary address into text
    IPbuffer = inet_ntoa(*((struct in_addr *) host_entry->h_addr_list[0]));
    return IPbuffer;
}