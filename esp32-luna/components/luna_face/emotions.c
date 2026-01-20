/*
 * Luna Emotions Configuration
 * Ported from luna_face_renderer.py EMOTIONS dictionary
 */

#include "emotions.h"
#include <string.h>
#include <math.h>

// Emotion configurations from Python luna_face_renderer.py
static const emotion_config_t s_emotions[EMOTION_COUNT] = {
    // EMOTION_EYES_ONLY (default - just eyes, no mouth)
    {
        .eye_height = 60.0f,
        .eye_width = 40.0f,
        .eye_openness = 1.0f,
        .mouth_curve = 0.0f,
        .mouth_open = 0.0f,
        .mouth_width = 0.0f,
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = true,
    },
    // EMOTION_NEUTRAL
    {
        .eye_height = 60.0f,
        .eye_width = 40.0f,
        .eye_openness = 1.0f,
        .mouth_curve = 0.0f,
        .mouth_open = 0.0f,
        .mouth_width = 40.0f,   // Wider mouth
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_HAPPY
    {
        .eye_height = 55.0f,
        .eye_width = 40.0f,
        .eye_openness = 0.85f,
        .mouth_curve = 0.8f,   // Bigger smile
        .mouth_open = 0.0f,
        .mouth_width = 50.0f,  // Much wider smile
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_SAD
    {
        .eye_height = 55.0f,
        .eye_width = 38.0f,
        .eye_openness = 0.8f,
        .mouth_curve = -0.9f,  // Much more pronounced frown
        .mouth_open = 0.0f,
        .mouth_width = 55.0f,  // Much wider frown
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_ANGRY
    {
        .eye_height = 45.0f,
        .eye_width = 45.0f,
        .eye_openness = 0.5f,
        .mouth_curve = -0.5f,  // More pronounced frown
        .mouth_open = 0.0f,
        .mouth_width = 45.0f,  // Wider
        .angry_brows = true,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_SURPRISED
    {
        .eye_height = 65.0f,
        .eye_width = 45.0f,
        .eye_openness = 1.15f,
        .mouth_curve = 0.0f,
        .mouth_open = 0.6f,    // Bigger O
        .mouth_width = 35.0f,  // Wider
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_THINKING
    {
        .eye_height = 55.0f,
        .eye_width = 40.0f,
        .eye_openness = 0.9f,
        .mouth_curve = 0.3f,   // Slight smile
        .mouth_open = 0.0f,
        .mouth_width = 35.0f,  // Wider
        .angry_brows = false,
        .look_side = true,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_CONFUSED
    {
        .eye_height = 60.0f,
        .eye_width = 40.0f,
        .eye_openness = 1.0f,
        .mouth_curve = -0.3f,  // Slight frown
        .mouth_open = 0.0f,
        .mouth_width = 35.0f,  // Wider
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = true,
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_EXCITED
    {
        .eye_height = 65.0f,
        .eye_width = 48.0f,
        .eye_openness = 1.2f,
        .mouth_curve = 1.0f,   // Big smile
        .mouth_open = 0.2f,
        .mouth_width = 55.0f,  // Very wide smile
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = true,
        .cat_face = false,
        .no_mouth = false,
    },
    // EMOTION_CAT
    {
        .eye_height = 60.0f,
        .eye_width = 40.0f,
        .eye_openness = 1.0f,
        .mouth_curve = 0.5f,
        .mouth_open = 0.0f,
        .mouth_width = 40.0f,
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = false,
        .sparkle = false,
        .cat_face = true,
        .no_mouth = false,
    },
    // EMOTION_DIZZY (from being shaken) - wide wobbly eyes, confused mouth
    {
        .eye_height = 65.0f,
        .eye_width = 45.0f,
        .eye_openness = 1.1f,
        .mouth_curve = -0.2f,   // Slight frown/confused
        .mouth_open = 0.3f,     // Slightly open "woozy" mouth
        .mouth_width = 40.0f,
        .angry_brows = false,
        .look_side = false,
        .tilt_eyes = true,      // Eyes at different heights (disoriented)
        .sparkle = false,
        .cat_face = false,
        .no_mouth = false,
    },
};

static const char *s_emotion_names[EMOTION_COUNT] = {
    "eyes_only",
    "neutral",
    "happy",
    "sad",
    "angry",
    "surprised",
    "thinking",
    "confused",
    "excited",
    "cat",
    "dizzy",
};

const emotion_config_t* emotion_get_config(emotion_id_t id)
{
    if (id < 0 || id >= EMOTION_COUNT) {
        return &s_emotions[EMOTION_EYES_ONLY];
    }
    return &s_emotions[id];
}

emotion_id_t emotion_from_string(const char *name)
{
    if (name == NULL) {
        return EMOTION_EYES_ONLY;
    }

    for (int i = 0; i < EMOTION_COUNT; i++) {
        if (strcasecmp(name, s_emotion_names[i]) == 0) {
            return (emotion_id_t)i;
        }
    }

    return EMOTION_EYES_ONLY;
}

const char* emotion_to_string(emotion_id_t id)
{
    if (id < 0 || id >= EMOTION_COUNT) {
        return s_emotion_names[EMOTION_EYES_ONLY];
    }
    return s_emotion_names[id];
}

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

void emotion_interpolate(const emotion_config_t *from,
                         const emotion_config_t *to,
                         float t,
                         emotion_config_t *result)
{
    if (from == NULL || to == NULL || result == NULL) {
        return;
    }

    // Clamp t to [0, 1]
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Interpolate numeric values
    result->eye_height = lerp(from->eye_height, to->eye_height, t);
    result->eye_width = lerp(from->eye_width, to->eye_width, t);
    result->eye_openness = lerp(from->eye_openness, to->eye_openness, t);
    result->mouth_curve = lerp(from->mouth_curve, to->mouth_curve, t);
    result->mouth_open = lerp(from->mouth_open, to->mouth_open, t);
    result->mouth_width = lerp(from->mouth_width, to->mouth_width, t);

    // Boolean flags use the target value after halfway
    bool use_target = t >= 0.5f;
    result->angry_brows = use_target ? to->angry_brows : from->angry_brows;
    result->look_side = use_target ? to->look_side : from->look_side;
    result->tilt_eyes = use_target ? to->tilt_eyes : from->tilt_eyes;
    result->sparkle = use_target ? to->sparkle : from->sparkle;
    result->cat_face = use_target ? to->cat_face : from->cat_face;
    result->no_mouth = use_target ? to->no_mouth : from->no_mouth;
}
