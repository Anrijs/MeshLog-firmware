#pragma once

#include <Arduino.h>

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
    uint32_t n = 0;
    while (*sp && *sp >= '0' && *sp <= '9') {
        n *= 10;
        n += (*sp++ - '0');
    }
    return n;
}

/* -------------------------------------------------------------------------------------- */


std::vector<String> split(const char* input, size_t limit) {
    std::vector<String> tokens;
    String current = "";
    
    while (*input != '\0') {
        if (limit > 0 && tokens.size() == limit - 1) {
            while (*input != '\0' && isSpace(*input)) {
                input++;
            }
            current += input;
            break;
        }
        
        if (isSpace(*input)) {
            if (current.length() > 0) {
                tokens.push_back(current);
                current = "";
            }
        } else {
            current += *input;
        }
        input++;
    }
    
    if (current.length() > 0) {
        tokens.push_back(current);
    }
    
    return tokens;
}
