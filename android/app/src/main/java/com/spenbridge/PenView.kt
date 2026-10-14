package com.spenbridge

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.os.Build
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.View
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Full-screen pen surface. Reads S Pen hover + contact through the standard
 * Android stylus APIs (unbuffered dispatch, historical samples, pressure,
 * tilt/orientation, distance, side button, eraser tool) and streams them to the PC.
 * Finger input is ignored completely (palm rejection).
 */
@SuppressLint("ViewConstructor")
class PenView(context: Context, private val prefs: Prefs) : View(context) {

    var monitorW = 1920
        set(v) { field = v; computeArea() }
    var monitorH = 1080
        set(v) { field = v; computeArea() }

    var statusLine = ""
        set(v) { if (field != v) { field = v; invalidate() } }

    private val area = RectF()
    private val density = resources.displayMetrics.density

    // ---- pen state ----
    private var penDown = false
    private var hovering = false
    private val hoverPos = PointF(-1f, -1f)
    private var lastFlagsSent = 0
    private val exitRunnable = Runnable { sendOutOfRange() }

    // ---- banner (e.g. "press 2 more times to exit") ----
    private var banner = ""
    private var bannerUntil = 0L

    // ---- ink preview trail ----
    private val trailX = FloatArray(TRAIL_MAX)
    private val trailY = FloatArray(TRAIL_MAX)
    private val trailP = FloatArray(TRAIL_MAX)
    private val trailT = LongArray(TRAIL_MAX)
    private val trailBreak = BooleanArray(TRAIL_MAX)
    private var trailHead = 0
    private var trailCount = 0

    private val bgPaint = Paint().apply { color = Color.rgb(7, 9, 13) }
    private val areaFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(13, 17, 24) }
    private val areaStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 1.2f * density; color = Color.argb(150, 52, 66, 90)
    }
    private val cornerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2.5f * density; strokeCap = Paint.Cap.ROUND
        color = Color.argb(200, 56, 189, 248)
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(110, 200, 214, 235); textSize = 12f * density
    }
    private val bannerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(235, 232, 238, 248); textSize = 18f * density; textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val bannerBg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(200, 16, 22, 32) }
    private val inkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeCap = Paint.Cap.ROUND; color = Color.rgb(125, 211, 252)
    }
    private val ringStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 1.6f * density; color = Color.argb(210, 220, 240, 255)
    }
    private val ringFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(56, 189, 248) }
    private val dotHalo = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }

    private val packBuf = ByteBuffer.allocate(Proto.MAX_SAMPLES * Proto.SAMPLE_SIZE).order(ByteOrder.LITTLE_ENDIAN)

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        setLayerType(LAYER_TYPE_HARDWARE, null)
    }

    // ------------------------------------------------------------------ layout

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        computeArea()
        // Keep side-swipe system gestures away from the canvas (system caps this at 200dp/edge).
        systemGestureExclusionRects = listOf(Rect(0, 0, w, h))
    }

    fun computeArea() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0 || h <= 0) return
        if (!prefs.keepAspect || monitorW <= 0 || monitorH <= 0) {
            area.set(0f, 0f, w, h)
        } else {
            val target = monitorW.toFloat() / monitorH
            if (w / h > target) {
                val aw = h * target
                area.set((w - aw) / 2f, 0f, (w + aw) / 2f, h)
            } else {
                val ah = w / target
                area.set(0f, (h - ah) / 2f, w, (h + ah) / 2f)
            }
        }
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        // Deliver every pointer sample (hover included) the moment it arrives instead of
        // batching once per frame: this is the single biggest latency win for a pen bridge.
        if (Build.VERSION.SDK_INT >= 31) requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(exitRunnable)
        if (hovering || penDown) sendOutOfRange()
        super.onDetachedFromWindow()
    }

    // Hide the system / Samsung hover pointer over our surface; we draw our own.
    override fun onResolvePointerIcon(event: MotionEvent, pointerIndex: Int): PointerIcon =
        PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL)

    // ------------------------------------------------------------------ input

    override fun onHoverEvent(e: MotionEvent): Boolean {
        val pi = penIndex(e)
        if (pi < 0) return false
        when (e.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                removeCallbacks(exitRunnable)
                val entering = !hovering
                hovering = true
                if (!penDown) emit(e, pi, contact = false, important = entering)
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                // Samsung fires HOVER_EXIT right before ACTION_DOWN; wait a moment so a
                // touch-down does not look like "left range, came back" on the PC.
                removeCallbacks(exitRunnable)
                postDelayed(exitRunnable, EXIT_GRACE_MS)
            }
        }
        return true
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val pi = penIndex(e)
        if (pi < 0) return true // fingers / palm: swallow and ignore
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (e.actionIndex == pi && !penDown) {
                    removeCallbacks(exitRunnable)
                    requestUnbufferedDispatch(e)
                    penDown = true
                    hovering = true
                    breakTrail()
                    emit(e, pi, contact = true, important = true)
                }
            }
            MotionEvent.ACTION_MOVE -> if (penDown) emit(e, pi, contact = true, important = false)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                if (penDown && (e.actionMasked == MotionEvent.ACTION_CANCEL || e.actionIndex == pi)) {
                    penDown = false
                    emit(e, pi, contact = false, important = true, includeHistory = false)
                    // If no hover events follow (pen pulled away fast), leave range.
                    removeCallbacks(exitRunnable)
                    postDelayed(exitRunnable, AFTER_UP_GRACE_MS)
                }
            }
        }
        return true
    }

    private fun penIndex(e: MotionEvent): Int {
        for (i in 0 until e.pointerCount) {
            val t = e.getToolType(i)
            if (t == MotionEvent.TOOL_TYPE_STYLUS || t == MotionEvent.TOOL_TYPE_ERASER) return i
        }
        return -1
    }

    private fun emit(e: MotionEvent, pi: Int, contact: Boolean, important: Boolean, includeHistory: Boolean = true) {
        val eraser = e.getToolType(pi) == MotionEvent.TOOL_TYPE_ERASER
        val bs = e.buttonState
        val barrel = (bs and (MotionEvent.BUTTON_STYLUS_PRIMARY or MotionEvent.BUTTON_STYLUS_SECONDARY or
            MotionEvent.BUTTON_SECONDARY)) != 0
        var flags = Proto.F_IN_RANGE
        if (contact) flags = flags or Proto.F_CONTACT
        if (barrel) flags = flags or Proto.F_BARREL
        if (eraser) flags = flags or Proto.F_ERASER

        packBuf.clear()
        var n = 0
        val hist = if (includeHistory) min(e.historySize, Proto.MAX_SAMPLES - 1) else 0
        val firstHist = e.historySize - hist
        for (h in firstHist until e.historySize) {
            putSample(
                flags, e.getHistoricalX(pi, h), e.getHistoricalY(pi, h),
                if (contact) e.getHistoricalPressure(pi, h) else 0f,
                e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, pi, h),
                e.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, pi, h),
                e.getHistoricalAxisValue(MotionEvent.AXIS_DISTANCE, pi, h),
            )
            n++
            if (contact) addTrail(e.getHistoricalX(pi, h), e.getHistoricalY(pi, h), e.getHistoricalPressure(pi, h))
        }
        val x = e.getX(pi)
        val y = e.getY(pi)
        val p = if (contact) e.getPressure(pi) else 0f
        putSample(
            flags, x, y, p, e.getAxisValue(MotionEvent.AXIS_TILT, pi),
            e.getAxisValue(MotionEvent.AXIS_ORIENTATION, pi), e.getAxisValue(MotionEvent.AXIS_DISTANCE, pi),
        )
        n++
        if (contact) addTrail(x, y, p)

        val stateChanged = flags != lastFlagsSent
        lastFlagsSent = flags
        val out = ByteArray(n * Proto.SAMPLE_SIZE)
        packBuf.flip()
        packBuf.get(out)
        PenLink.sendPen(out, n, important || stateChanged)

        hoverPos.set(x, y)
        postInvalidateOnAnimation()
    }

    private fun putSample(flags: Int, x: Float, y: Float, pressure: Float, tilt: Float, orientation: Float, distance: Float) {
        val nx = ((x - area.left) / max(1f, area.width())).coerceIn(0f, 1f)
        val ny = ((y - area.top) / max(1f, area.height())).coerceIn(0f, 1f)
        // Pressure may exceed 1.0 depending on calibration; normalise and keep a floor
        // while touching so the PC never sees "contact with zero pressure".
        val p = if ((flags and Proto.F_CONTACT) != 0) pressure.coerceIn(0.002f, 1f) else 0f
        // Android: tilt = angle from perpendicular, orientation = azimuth (0 = up, +pi/2 = right).
        // Convert to the tiltX / tiltY pair Windows Ink (and the W3C pointer spec) use.
        val r = sin(tilt)
        val z = cos(tilt)
        val tx = Math.toDegrees(atan2(sin(orientation) * r, z).toDouble()).roundToInt().coerceIn(-90, 90)
        val ty = Math.toDegrees(atan2(-cos(orientation) * r, z).toDouble()).roundToInt().coerceIn(-90, 90)
        packBuf.put(flags.toByte())
        packBuf.put(distance.coerceIn(0f, 255f).toInt().toByte())
        packBuf.putShort((p * 65535f).toInt().toShort())
        packBuf.putFloat(nx)
        packBuf.putFloat(ny)
        packBuf.put(tx.toByte())
        packBuf.put(ty.toByte())
        packBuf.putShort(0)
    }

    private fun sendOutOfRange() {
        removeCallbacks(exitRunnable)
        if (penDown) return
        hovering = false
        lastFlagsSent = 0
        packBuf.clear()
        packBuf.put(0).put(0).putShort(0)
        val nx = ((hoverPos.x - area.left) / max(1f, area.width())).coerceIn(0f, 1f)
        val ny = ((hoverPos.y - area.top) / max(1f, area.height())).coerceIn(0f, 1f)
        packBuf.putFloat(nx).putFloat(ny).put(0).put(0).putShort(0)
        val out = ByteArray(Proto.SAMPLE_SIZE)
        packBuf.flip()
        packBuf.get(out)
        PenLink.sendPen(out, 1, true)
        invalidate()
    }

    /** Called when the activity loses focus so the PC never keeps a stuck pen. */
    fun releasePen() {
        penDown = false
        sendOutOfRange()
    }

    fun showBanner(text: String, ms: Long) {
        banner = text
        bannerUntil = SystemClock.uptimeMillis() + ms
        invalidate()
        postDelayed({ invalidate() }, ms + 16)
    }

    // ------------------------------------------------------------------ ink trail

    private fun addTrail(x: Float, y: Float, p: Float) {
        if (!prefs.inkTrail) return
        trailX[trailHead] = x
        trailY[trailHead] = y
        trailP[trailHead] = p.coerceIn(0f, 1f)
        trailT[trailHead] = SystemClock.uptimeMillis()
        trailBreak[trailHead] = false
        trailHead = (trailHead + 1) % TRAIL_MAX
        if (trailCount < TRAIL_MAX) trailCount++
    }

    private fun breakTrail() {
        if (trailCount == 0) return
        val last = (trailHead - 1 + TRAIL_MAX) % TRAIL_MAX
        trailBreak[last] = true
    }

    // ------------------------------------------------------------------ drawing

    override fun onDraw(canvas: Canvas) {
        val now = SystemClock.uptimeMillis()
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bgPaint)
        val rad = 10f * density
        canvas.drawRoundRect(area, rad, rad, areaFill)
        canvas.drawRoundRect(area, rad, rad, areaStroke)
        drawCorners(canvas)

        val pad = 14f * density
        canvas.drawText(statusLine, area.left + pad, area.top + pad + textPaint.textSize, textPaint)
        val hint = "Volume Up ×3 to exit"
        canvas.drawText(hint, area.right - pad - textPaint.measureText(hint), area.bottom - pad, textPaint)

        var animating = drawTrail(canvas, now)

        if (hovering && hoverPos.x >= 0) drawHoverLens(canvas)

        if (now < bannerUntil && banner.isNotEmpty()) {
            val tw = bannerPaint.measureText(banner)
            val cx = width / 2f
            val cy = height / 2f
            val bw = tw / 2f + 22f * density
            canvas.drawRoundRect(cx - bw, cy - 28f * density, cx + bw, cy + 22f * density, 18f * density, 18f * density, bannerBg)
            canvas.drawText(banner, cx, cy + 4f * density, bannerPaint)
            animating = true
        }
        if (animating) postInvalidateOnAnimation()
    }

    private fun drawCorners(c: Canvas) {
        val l = 18f * density
        val a = area
        c.drawLine(a.left, a.top + l, a.left, a.top, cornerPaint); c.drawLine(a.left, a.top, a.left + l, a.top, cornerPaint)
        c.drawLine(a.right - l, a.top, a.right, a.top, cornerPaint); c.drawLine(a.right, a.top, a.right, a.top + l, cornerPaint)
        c.drawLine(a.left, a.bottom - l, a.left, a.bottom, cornerPaint); c.drawLine(a.left, a.bottom, a.left + l, a.bottom, cornerPaint)
        c.drawLine(a.right - l, a.bottom, a.right, a.bottom, cornerPaint); c.drawLine(a.right, a.bottom, a.right, a.bottom - l, cornerPaint)
    }

    private fun drawTrail(c: Canvas, now: Long): Boolean {
        if (trailCount < 2) return false
        var any = false
        val start = (trailHead - trailCount + TRAIL_MAX) % TRAIL_MAX
        var prev = start
        for (k in 1 until trailCount) {
            val i = (start + k) % TRAIL_MAX
            val age = now - trailT[i]
            if (age < TRAIL_MS && !trailBreak[prev]) {
                val fade = 1f - age.toFloat() / TRAIL_MS
                inkPaint.alpha = (fade * fade * 230).toInt()
                inkPaint.strokeWidth = (0.8f + 3.2f * trailP[i]) * density
                c.drawLine(trailX[prev], trailY[prev], trailX[i], trailY[i], inkPaint)
                any = true
            }
            prev = i
        }
        if (!any) trailCount = 0
        return any
    }

    private fun drawHoverLens(c: Canvas) {
        val r = (if (penDown) 9f else 13f) * density
        val x = hoverPos.x
        val y = hoverPos.y
        ringFill.shader = RadialGradient(
            x - r * 0.35f, y - r * 0.45f, r * 1.4f,
            intArrayOf(Color.argb(70, 255, 255, 255), Color.argb(18, 160, 220, 255), Color.argb(40, 56, 189, 248)),
            floatArrayOf(0f, 0.6f, 1f), Shader.TileMode.CLAMP,
        )
        c.drawCircle(x, y, r, ringFill)
        c.drawCircle(x, y, r, ringStroke)
        c.drawCircle(x, y, 2.6f * density, dotHalo)
        c.drawCircle(x, y, 1.6f * density, dotPaint)
    }

    private companion object {
        const val TRAIL_MAX = 1024
        const val TRAIL_MS = 650L
        const val EXIT_GRACE_MS = 120L
        const val AFTER_UP_GRACE_MS = 250L
    }
}
