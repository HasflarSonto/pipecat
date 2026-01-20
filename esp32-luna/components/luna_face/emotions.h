/*
 * Luna Emotions Configuration
 * Ported from luna_face_renderer.py EMOTIONS dictionary
 */

#ifndef LUNA_EMOTIONS_H
#define LUNA_EMOTIONS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Emotion configuration structure
 * Matches Python luna_face_renderer.py EMOTIONS dict
 */
typedef struct {
    float eye_height;      // Taller = more alert (default ~60)
    float eye_width;       // Wider = more surprised (default ~40)
    float eye_openness;    // 0-1, affects blink depth (default 1.0)
    float mouth_curve;     // Positive = smile, negative = frown (default 0.0)
    float mouth_open;      // 0-1, for surprised "O" shape (default 0.0)
    float mouth_width;     // Mouth horizontal size (default ~25)
    bool angry_brows;      // Angled eyebrows for angry
    bool look_side;        // Eyes look sideways (thinking)
    bool tilt_eyes;        // One eye higher than other (confused)
    bool sparkle;          // Sparkle effect (excited)
    bool cat_face;         // Cat-style mouth (":3" shape)
    bool no_mouth;         // Eyes only, no mouth displayed
} emotion_config_t;

/**
 * @brief Emotion identifiers
 */
typedef enum {
    EMOTION_EYES_ONLY = 0,  // Default: just eyes, no mouth
    EMOTION_NEUTRAL,
    EMOTION_HAPPY,
    EMOTION_SAD,
    EMOTION_ANGRY,
    EMOTION_SURPRISED,
    EMOTION_THINKING,
    EMOTION_CONFUSED,
    EMOTION_EXCITED,
    EMOTION_CAT,
    EMOTION_DIZZY,          // Dizzy from being shaken - spiral eyes
    EMOTION_COUNT
} emotion_id_t;

/**
 * @brief Get emotion configuration by ID
 * @param id Emotion identifier
 * @return Pointer to emotion config (static, do not free)
 */
const emotion_config_t* emotion_get_config(emotion_id_t id);

/**
 * @brief Get emotion ID from string name
 * @param name Emotion name (e.g., "happy", "sad")
 * @return Emotion ID, or EMOTION_NEUTRAL if not found
 */
emotion_id_t emotion_from_string(const char *name);

/**
 * @brief Get emotion name from ID
 * @param id Emotion identifier
 * @return Static string with emotion name
 */
const char* emotion_to_string(emotion_id_t id);

/**
 * @brief Interpolate between two emotion configurations
 * @param from Source emotion config
 * @param to Target emotion config
 * @param t Interpolation factor (0.0 = from, 1.0 = to)
 * @param result Output interpolated config
 */
void emotion_interpolate(const emotion_config_t *from,
                         const emotion_config_t *to,
                         float t,
                         emotion_config_t *result);

/*
 * Default emotion configurations (from luna_face_renderer.py)
 *
 * neutral:   eye_height=60, eye_width=40, eye_openness=1.0, mouth_curve=0.0, mouth_open=0.0, mouth_width=25
 * happy:     eye_height=55, eye_width=40, eye_openness=0.85, mouth_curve=0.5, mouth_open=0.0, mouth_width=22
 * sad:       eye_height=55, eye_width=38, eye_openness=0.8, mouth_curve=-0.4, mouth_open=0.0, mouth_width=20
 * angry:     eye_height=45, eye_width=45, eye_openness=0.5, mouth_curve=-0.3, mouth_open=0.0, mouth_width=25, angry_brows=true
 * surprised: eye_height=65, eye_width=45, eye_openness=1.15, mouth_curve=0.0, mouth_open=0.4, mouth_width=20
 * thinking:  eye_height=55, eye_width=40, eye_openness=0.9, mouth_curve=0.2, mouth_open=0.0, mouth_width=18, look_side=true
 * confused:  eye_height=60, eye_width=40, eye_openness=1.0, mouth_curve=-0.2, mouth_open=0.0, mouth_width=20, tilt_eyes=true
 * excited:   eye_height=65, eye_width=48, eye_openness=1.2, mouth_curve=0.8, mouth_open=0.2, mouth_width=30, sparkle=true
 * cat:       eye_height=60, eye_width=40, eye_openness=1.0, mouth_curve=0.5, mouth_open=0.0, mouth_width=40, cat_face=true
 */

#ifdef __cplusplus
}
#endif

#endif // LUNA_EMOTIONS_H
