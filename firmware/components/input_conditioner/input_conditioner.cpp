#include "input_conditioner.h"
#include "blinker.h"

#include <atomic>
#include "driver/gpio.h"
#include "esp_timer.h"

namespace InputConditioner
{

namespace
{

/* Packed flag bits published to the reader. */
constexpr uint32_t F_LEFT_BLINK = 1u << 0;
constexpr uint32_t F_LEFT_ON = 1u << 1;
constexpr uint32_t F_RIGHT_BLINK = 1u << 2;
constexpr uint32_t F_RIGHT_ON = 1u << 3;
constexpr uint32_t F_BRAKE = 1u << 4;
constexpr uint32_t F_AUX = 1u << 5;
constexpr uint32_t F_LEARNED = 1u << 6;
constexpr uint32_t F_BRAKE_INTRO = 1u << 7;

/* Simulated signals: injected as raw inputs (see forceEvent). */
constexpr uint32_t FORCE_TTL_MS = 60000;
constexpr uint32_t OVERRIDE_TTL_MS = 60000;

static Blink::BlinkSystem m_sys;                 /* touched only by the timer cb */
static esp_timer_handle_t m_timer;
static InputPins m_pins;

static std::atomic<uint32_t> m_flags;
static std::atomic<uint32_t> m_left_phase, m_right_phase, m_brake_edge, m_aux_edge;
static std::atomic<uint32_t> m_left_blink_start, m_right_blink_start;
static std::atomic<uint32_t> m_period;
static std::atomic<uint32_t> m_dirty_period;     /* 0 = clean, else value to persist */
static std::atomic<uint32_t> m_brake_holdoff{ Blink::BRAKE_HOLDOFF_MS };
static std::atomic<uint8_t>  m_exit_x10;         /* 0 = keep the init-time value */
static std::atomic<uint32_t> m_force_until[COND_EVENT_COUNT];
static std::atomic<uint32_t> m_force_start[COND_EVENT_COUNT];
static std::atomic<uint32_t> m_override_until;

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool ttlActive(const uint32_t until, const uint32_t now)
{
    return 0 != until && static_cast<int32_t>(until - now) > 0;
}

/* Synthetic flasher wave for a forced turn signal. */
bool forceWave(const CondEvent ev, const uint32_t now)
{
    const int i = static_cast<int>(ev);
    const uint32_t start = m_force_start[i].load(std::memory_order_relaxed);
    const uint32_t period = (0 != m_sys.period_ms) ? m_sys.period_ms
                                                   : Blink::PERIOD_DEFAULT_MS;
    return ((now - start) % period) < period / 2;
}

bool forcedNow(const CondEvent ev, const uint32_t now)
{
    const int i = static_cast<int>(ev);
    return ttlActive(m_force_until[i].load(std::memory_order_relaxed), now);
}

void sampleCb(void *arg)
{
    (void)arg;
    const uint32_t now_ms = nowMs();

    m_sys.brake_holdoff_ms = m_brake_holdoff.load(std::memory_order_relaxed);
    const uint8_t exit_x10 = m_exit_x10.load(std::memory_order_relaxed);
    if (0 != exit_x10)
    {
        m_sys.exit_x10 = exit_x10;
    }

    /* Active-low: 12V signal ON -> opto pulls the GPIO to 0. */
    bool l = 0 == gpio_get_level(static_cast<gpio_num_t>(m_pins.left));
    bool r = 0 == gpio_get_level(static_cast<gpio_num_t>(m_pins.right));
    bool b = 0 == gpio_get_level(static_cast<gpio_num_t>(m_pins.brake));
    bool x = 0 == gpio_get_level(static_cast<gpio_num_t>(m_pins.aux));

    /* Simulated signals enter HERE, before any logic: override masks the
     * real inputs, forced turns pulse like a real flasher, forced brake/aux
     * are steady. Everything downstream (debounce, blink tracking, brake
     * holdoff) treats them exactly like the real thing. */
    if (ttlActive(m_override_until.load(std::memory_order_relaxed), now_ms))
    {
        l = r = b = x = false;
    }
    if (forcedNow(CondEvent::Left, now_ms))
    {
        l |= forceWave(CondEvent::Left, now_ms);
    }
    if (forcedNow(CondEvent::Right, now_ms))
    {
        r |= forceWave(CondEvent::Right, now_ms);
    }
    if (forcedNow(CondEvent::Brake, now_ms))
    {
        b = true;
    }
    if (forcedNow(CondEvent::Aux, now_ms))
    {
        x = true;
    }

    Blink::tick(&m_sys, l, r, b, x, now_ms);

    uint32_t f = 0;
    if (m_sys.left.blink_mode)   { f |= F_LEFT_BLINK; }
    if (m_sys.left.debounced)    { f |= F_LEFT_ON; }
    if (m_sys.right.blink_mode)  { f |= F_RIGHT_BLINK; }
    if (m_sys.right.debounced)   { f |= F_RIGHT_ON; }
    if (m_sys.brake.debounced)   { f |= F_BRAKE; }
    if (m_sys.aux.debounced)     { f |= F_AUX; }
    if (m_sys.learned)           { f |= F_LEARNED; }
    if (m_sys.brake_intro)       { f |= F_BRAKE_INTRO; }

    m_left_phase.store(m_sys.left.last_phase_edge_ms, std::memory_order_relaxed);
    m_right_phase.store(m_sys.right.last_phase_edge_ms, std::memory_order_relaxed);
    m_left_blink_start.store(m_sys.left.blink_start_ms, std::memory_order_relaxed);
    m_right_blink_start.store(m_sys.right.blink_start_ms, std::memory_order_relaxed);
    m_brake_edge.store(m_sys.brake.last_phase_edge_ms, std::memory_order_relaxed);
    m_aux_edge.store(m_sys.aux.last_phase_edge_ms, std::memory_order_relaxed);
    m_period.store(m_sys.period_ms, std::memory_order_relaxed);
    m_flags.store(f, std::memory_order_release);

    if (m_sys.period_dirty)
    {
        m_sys.period_dirty = false;
        m_dirty_period.store(m_sys.period_ms, std::memory_order_relaxed);
    }
}

} // namespace

esp_err_t init(const InputPins &pins, const uint32_t stored_period_ms,
               const uint8_t exit_x10)
{
    m_pins = pins;

    /* Inputs are active low behind the PCB's optocouplers, which have their
     * own 10k pull-ups to 3V3. Enable the internal pull-up as well wherever
     * the pad has one: a disconnected or broken input then still reads
     * "signal off" instead of floating. GPIO 34-39 are input-only pads with
     * no pull resistors at all, so they rely on the external ones. */
    const int all_pins[] = { pins.left, pins.right, pins.brake, pins.aux };
    uint64_t pull_mask = 0;
    uint64_t plain_mask = 0;
    for (const int pin : all_pins)
    {
        if (GPIO_IS_VALID_OUTPUT_GPIO(pin))
        {
            pull_mask |= 1ULL << pin;
        }
        else
        {
            plain_mask |= 1ULL << pin;
        }
    }

    gpio_config_t io = {};
    io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = ESP_OK;
    if (0 != pull_mask)
    {
        io.pin_bit_mask = pull_mask;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        err = gpio_config(&io);
        if (ESP_OK != err)
        {
            return err;
        }
    }
    if (0 != plain_mask)
    {
        io.pin_bit_mask = plain_mask;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        err = gpio_config(&io);
        if (ESP_OK != err)
        {
            return err;
        }
    }

    Blink::init(&m_sys, stored_period_ms, exit_x10);

    esp_timer_create_args_t targs = {};
    targs.callback = sampleCb;
    targs.name = "input_cond";
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.skip_unhandled_events = true;
    err = esp_timer_create(&targs, &m_timer);
    if (ESP_OK != err)
    {
        return err;
    }
    return esp_timer_start_periodic(m_timer, 1000); /* 1 ms */
}

void get(CondState *out)
{
    const uint32_t f = m_flags.load(std::memory_order_acquire);
    out->left_blink  = 0 != (f & F_LEFT_BLINK);
    out->left_on     = 0 != (f & F_LEFT_ON);
    out->right_blink = 0 != (f & F_RIGHT_BLINK);
    out->right_on    = 0 != (f & F_RIGHT_ON);
    out->brake       = 0 != (f & F_BRAKE);
    out->brake_intro = 0 != (f & F_BRAKE_INTRO);
    out->aux         = 0 != (f & F_AUX);
    out->learned     = 0 != (f & F_LEARNED);
    out->left_phase_ms  = m_left_phase.load(std::memory_order_relaxed);
    out->right_phase_ms = m_right_phase.load(std::memory_order_relaxed);
    out->left_blink_start_ms  = m_left_blink_start.load(std::memory_order_relaxed);
    out->right_blink_start_ms = m_right_blink_start.load(std::memory_order_relaxed);
    out->brake_edge_ms  = m_brake_edge.load(std::memory_order_relaxed);
    out->aux_edge_ms    = m_aux_edge.load(std::memory_order_relaxed);
    out->period_ms      = m_period.load(std::memory_order_relaxed);
}

bool takeDirtyPeriod(uint32_t *period_ms)
{
    const uint32_t v = m_dirty_period.exchange(0, std::memory_order_relaxed);
    if (0 == v)
    {
        return false;
    }
    *period_ms = v;
    return true;
}

void setBrakeHoldoff(const uint32_t ms)
{
    m_brake_holdoff.store(ms, std::memory_order_relaxed);
}

void setExitFactor(const uint8_t x10)
{
    m_exit_x10.store(x10, std::memory_order_relaxed);
}

void forceEvent(const CondEvent ev, const bool active)
{
    const int i = static_cast<int>(ev);
    if (i >= COND_EVENT_COUNT)
    {
        return;
    }
    const uint32_t now = nowMs();
    if (active)
    {
        uint32_t start = now;
        /* Turn signals: if the other side is already forced, share its wave
         * origin so both synthetic flashers pulse in phase (activating
         * hazard after a single blinker must not desync them). */
        if (CondEvent::Left == ev || CondEvent::Right == ev)
        {
            const int other = static_cast<int>(
                (CondEvent::Left == ev) ? CondEvent::Right : CondEvent::Left);
            if (ttlActive(m_force_until[other].load(), now))
            {
                start = m_force_start[other].load();
            }
        }
        m_force_start[i].store(start);
        m_force_until[i].store(now + FORCE_TTL_MS);
    }
    else
    {
        m_force_until[i].store(0);
    }
}

bool eventForced(const CondEvent ev)
{
    const int i = static_cast<int>(ev);
    if (i >= COND_EVENT_COUNT)
    {
        return false;
    }
    return ttlActive(m_force_until[i].load(), nowMs());
}

void setOverride(const bool active)
{
    m_override_until.store(active ? nowMs() + OVERRIDE_TTL_MS : 0);
}

bool overrideActive()
{
    return ttlActive(m_override_until.load(), nowMs());
}

} // namespace InputConditioner
