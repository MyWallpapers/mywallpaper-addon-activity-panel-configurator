#include "accessibility.hpp"
#include "panel.hpp"
#include <atomic>
namespace activity {
namespace {
class Accessible final : public IAccessible {
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<AccessibleState> state_;
    bool Valid(VARIANT child) const {
        return child.vt == VT_I4 && child.lVal >= 0 &&
               child.lVal <= static_cast<LONG>(state_->labels.size());
    }
    HRESULT String(const std::string &value, BSTR *out) {
        if (!out)
            return E_POINTER;
        auto w = Wide(value);
        *out = SysAllocStringLen(w.data(), static_cast<UINT>(w.size()));
        return *out ? S_OK : E_OUTOFMEMORY;
    }

  public:
    explicit Accessible(std::shared_ptr<AccessibleState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDispatch || id == IID_IAccessible) {
            *out = static_cast<IAccessible *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --refs_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *out) override {
        if (!out)
            return E_POINTER;
        *out = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **out) override {
        if (out)
            *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override {
        return DISP_E_UNKNOWNNAME;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *,
                                     EXCEPINFO *, UINT *) override {
        return DISP_E_MEMBERNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long *out) override {
        if (!out)
            return E_POINTER;
        std::lock_guard lock(state_->mutex);
        *out = static_cast<long>(state_->labels.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR *out) override {
        std::lock_guard lock(state_->mutex);
        if (!Valid(child))
            return E_INVALIDARG;
        return String(child.lVal ? state_->labels[child.lVal - 1] : state_->name, out);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR *out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR *out) override {
        std::lock_guard lock(state_->mutex);
        if (!Valid(child))
            return E_INVALIDARG;
        return String(
            child.lVal
                ? "Bouton du panneau d’activité"
                : "Tab ou flèches pour choisir, Entrée ou Espace pour agir, Échap pour réduire",
            out);
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT *out) override {
        if (!out)
            return E_POINTER;
        std::lock_guard lock(state_->mutex);
        if (!Valid(child))
            return E_INVALIDARG;
        VariantInit(out);
        out->vt = VT_I4;
        out->lVal = child.lVal ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_PANE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT *out) override {
        if (!out)
            return E_POINTER;
        std::lock_guard lock(state_->mutex);
        if (!Valid(child))
            return E_INVALIDARG;
        VariantInit(out);
        out->vt = VT_I4;
        out->lVal = STATE_SYSTEM_FOCUSABLE;
        if (!state_->window || !IsWindowVisible(state_->window))
            out->lVal |= STATE_SYSTEM_INVISIBLE;
        if (child.lVal == state_->focus + 1 && state_->focus >= 0)
            out->lVal |= STATE_SYSTEM_FOCUSED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR *out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR *file, VARIANT, long *topic) override {
        if (file)
            *file = nullptr;
        if (topic)
            *topic = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR *out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return child.lVal ? String("Entrée", out) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT *out) override {
        if (!out)
            return E_POINTER;
        std::lock_guard lock(state_->mutex);
        VariantInit(out);
        if (state_->focus >= 0) {
            out->vt = VT_I4;
            out->lVal = state_->focus + 1;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT *out) override {
        if (!out)
            return E_POINTER;
        VariantInit(out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR *out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        std::lock_guard lock(state_->mutex);
        if (!Valid(child))
            return E_INVALIDARG;
        return child.lVal ? String("Activer", out) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        std::lock_guard lock(state_->mutex);
        if (!Valid(child) || child.lVal == 0)
            return E_INVALIDARG;
        if (!(flags & SELFLAG_TAKEFOCUS) || !state_->window)
            return S_FALSE;
        return PostMessageW(state_->window, accessibleFocus,
                            (static_cast<WPARAM>(state_->revision) << 32) |
                                static_cast<uint32_t>(child.lVal - 1),
                            reinterpret_cast<LPARAM>(state_.get()))
                   ? S_OK
                   : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long *x, long *y, long *w, long *h,
                                          VARIANT child) override {
        if (!x || !y || !w || !h)
            return E_POINTER;
        std::lock_guard lock(state_->mutex);
        if (!Valid(child) || !state_->window)
            return E_INVALIDARG;
        RECT window{};
        if (!GetWindowRect(state_->window, &window))
            return E_FAIL;
        auto r = child.lVal ? state_->locations[child.lVal - 1]
                            : Rect{0, 0, window.right - window.left, window.bottom - window.top};
        *x = window.left + r.x;
        *y = window.top + r.y;
        *w = r.width;
        *h = r.height;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT *out) override {
        if (!out)
            return E_POINTER;
        VariantInit(out);
        std::lock_guard lock(state_->mutex);
        if (!Valid(start))
            return E_INVALIDARG;
        long child = 0;
        if (direction == NAVDIR_FIRSTCHILD && start.lVal == 0)
            child = 1;
        else if (direction == NAVDIR_LASTCHILD && start.lVal == 0)
            child = static_cast<long>(state_->labels.size());
        else if (direction == NAVDIR_NEXT)
            child = start.lVal + 1;
        else if (direction == NAVDIR_PREVIOUS)
            child = start.lVal - 1;
        if (child < 1 || child > static_cast<long>(state_->labels.size()))
            return S_FALSE;
        out->vt = VT_I4;
        out->lVal = child;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT *out) override {
        if (!out)
            return E_POINTER;
        VariantInit(out);
        std::lock_guard lock(state_->mutex);
        if (!state_->window)
            return S_FALSE;
        RECT window{};
        GetWindowRect(state_->window, &window);
        if (x < window.left || y < window.top || x >= window.right || y >= window.bottom)
            return S_FALSE;
        out->vt = VT_I4;
        out->lVal = CHILDID_SELF;
        for (size_t i = 0; i < state_->locations.size(); i++) {
            auto r = state_->locations[i];
            if (x >= window.left + r.x && y >= window.top + r.y &&
                x < window.left + r.x + r.width && y < window.top + r.y + r.height) {
                out->lVal = static_cast<LONG>(i) + 1;
                break;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        std::lock_guard lock(state_->mutex);
        if (!Valid(child) || child.lVal == 0 || !state_->window)
            return E_INVALIDARG;
        return PostMessageW(state_->window, accessibleAction,
                            (static_cast<WPARAM>(state_->revision) << 32) |
                                static_cast<uint32_t>(child.lVal - 1),
                            reinterpret_cast<LPARAM>(state_.get()))
                   ? S_OK
                   : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
};
} // namespace
LRESULT AccessibleObject(WPARAM request, const std::shared_ptr<AccessibleState> &state) {
    auto accessible = new Accessible(state);
    auto result = LresultFromObject(IID_IAccessible, request, accessible);
    accessible->Release();
    return result;
}
} // namespace activity
