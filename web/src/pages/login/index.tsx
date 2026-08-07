import { useEffect, useRef, useState } from 'react';
import { z } from 'zod';
import {
  ArrowRight,
  ChevronDown,
  Eye,
  EyeOff,
  Globe,
  Loader2,
} from 'lucide-react';
import { toast } from 'sonner';
import { authApi } from '@/services/api';
import { useAuthStore } from '@/store/auth';
import { setItem } from '@/utils/storage';
import { useTranslation } from 'react-i18next';
import { useRouter } from '@/router/hooks/use-router';
import { changeLanguage } from '@/i18n/utils';
import { consumeOTASuccessLoginMessage } from '@/utils/otaLoginRedirect';
import loginBg from '@/assets/images/login-bg.webp';
import darkLogo from '@/assets/images/dark_logo.svg';

interface LoginFormValues {
  username: string;
  password: string;
}

interface LoginFormErrors {
  username?: string;
  password?: string;
}

export default function Login() {
  const { t, i18n } = useTranslation();
  const { setToken } = useAuthStore();
  const router = useRouter();
  const [langMenuOpen, setLangMenuOpen] = useState(false);
  const langMenuRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const message = consumeOTASuccessLoginMessage(
      t('sys.device_info.ota_complete_login_prompt', '固件升级完成，请重新登录')
    );
    if (message) {
      toast.success(message);
    }
  }, [t]);

  useEffect(() => {
    if (!langMenuOpen) return;
    const handlePointerDown = (e: PointerEvent) => {
      if (!langMenuRef.current) return;
      if (!langMenuRef.current.contains(e.target as Node)) {
        setLangMenuOpen(false);
      }
    };
    window.addEventListener('pointerdown', handlePointerDown);
    return () => window.removeEventListener('pointerdown', handlePointerDown);
  }, [langMenuOpen]);

  const [values, setValues] = useState<LoginFormValues>({
    username: '',
    password: '',
  });
  const [errors, setErrors] = useState<LoginFormErrors>({});
  const [isLoading, setIsLoading] = useState(false);
  const [showPassword, setShowPassword] = useState(false);

  const handleLanguageChange = (value: string) => {
    if (value !== i18n.language) {
      changeLanguage(value);
    }
  };

  const validate = (): boolean => {
    const schema = z.object({
      username: z.string().min(1, { message: t('sys.login.username_error') }),
      password: z
        .string()
        .min(1, { message: t('sys.login.password_error') })
        .refine(value => !value.includes(' '), {
          message: t('sys.login.password_illegal_error'),
        }),
    });

    const result = schema.safeParse(values);

    if (!result.success) {
      const { fieldErrors } = result.error.flatten();
      setErrors({
        username: fieldErrors.username?.[0],
        password: fieldErrors.password?.[0],
      });
      return false;
    }

    setErrors({});
    return true;
  };

  const getLoginErrorMessage = (payload: unknown) => {
    const maybeResponse = payload as { status?: number; data?: unknown } | null;
    const raw = maybeResponse?.data ?? payload;

    const data = raw as {
      code?: number;
      message?: string;
      error?: { detail?: string };
    } | null;

    const detail = data?.error?.detail;
    if (detail) {
      if (detail === 'Invalid username or password') {
        return t(
          'sys.login.invalid_credentials',
          'Invalid username or password'
        );
      }
      return detail;
    }

    if (data?.code === 2000 || data?.message === 'Unauthorized') {
      return t('sys.login.invalid_credentials', 'Invalid username or password');
    }

    return data?.message || t('errors.business.default');
  };

  const handleLogin = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!validate()) return;

    setIsLoading(true);

    const { username, password } = values;
    try {
      const res = await authApi.login({
        username,
        password,
      });
      if (res && res.code === 0 && res.data) {
        setToken(res.data.token);
        if (res.data.username) {
          setItem('username', res.data.username);
        }
        router.push('/');
      } else {
        toast.error(getLoginErrorMessage(res));
      }
    } catch (error: unknown) {
      const err = error as
        | { response?: { status?: number; data?: unknown }; message?: string }
        | { status?: number; data?: unknown }
        | null;

      const payload = err
        ? 'data' in err
          ? err
          : 'response' in err
            ? err.response
            : undefined
        : undefined;
      if (payload) {
        toast.error(getLoginErrorMessage(payload));
      } else {
        toast.error((err as any)?.message || t('errors.business.default'));
      }

      console.error('Login error:', error);
    } finally {
      setIsLoading(false);
    }
  };

  return (
    <div className="relative min-h-dvh w-full overflow-hidden bg-[#f6f7f9] text-slate-900 antialiased">
      {/* Right: Illustration */}
      <div className="pointer-events-none absolute inset-0">
        <img
          src={loginBg}
          alt=""
          className="absolute inset-y-0 right-0 h-full w-auto max-w-none"
        />
        {/* Keep right side visible; fade for form readability */}
        <div className="absolute inset-0 bg-linear-to-r from-[#f6f7f9] via-[#f6f7f9]/60 to-transparent lg:via-[#f6f7f9]/45" />
      </div>

      {/* Top-left logo */}
      <div className="absolute top-6 left-6 z-10">
        <img src={darkLogo} alt="CamThink" className="h-10  w-auto" />
      </div>

      {/* Language control */}
      <div className="absolute top-6 right-6 z-10 flex items-center gap-2 text-slate-700">
        <div className="relative" ref={langMenuRef}>
          <button
            type="button"
            onClick={() => setLangMenuOpen(v => !v)}
            className="flex h-9 items-center justify-between gap-2 rounded-md border border-slate-200 bg-white/70 px-3 text-sm shadow-xs outline-none backdrop-blur transition-[color,box-shadow] hover:bg-white/90 focus-visible:ring-[3px] focus-visible:ring-[#f24a00]/20"
            aria-haspopup="menu"
            aria-expanded={langMenuOpen}
          >
            <Globe className="h-4 w-4 text-slate-600" />
            <span className="text-sm font-medium text-slate-800">
              {i18n.language === 'zh'
                ? t('common.language_zh', '简体中文')
                : 'English'}
            </span>
            <ChevronDown className="h-4 w-4 opacity-50" />
          </button>

          {langMenuOpen && (
            <div
              role="menu"
              className="absolute right-0 z-50 mt-2 w-36 overflow-hidden rounded-md border border-slate-200 bg-white text-slate-900 shadow-md"
            >
              <button
                type="button"
                role="menuitem"
                className={`flex w-full cursor-default items-center rounded-sm py-2 pr-8 pl-2 text-left text-sm outline-none hover:bg-slate-50 ${
                  i18n.language === 'zh'
                    ? 'bg-slate-50 font-medium text-slate-900'
                    : 'text-slate-700'
                }`}
                onClick={() => {
                  handleLanguageChange('zh');
                  setLangMenuOpen(false);
                }}
              >
                {t('common.language_zh', '简体中文')}
              </button>
              <button
                type="button"
                role="menuitem"
                className={`flex w-full cursor-default items-center rounded-sm py-1.5 pr-8 pl-2 text-left text-sm outline-none hover:bg-slate-50 ${
                  i18n.language === 'en'
                    ? 'bg-slate-50 font-medium text-slate-900'
                    : 'text-slate-700'
                }`}
                onClick={() => {
                  handleLanguageChange('en');
                  setLangMenuOpen(false);
                }}
              >
                English
              </button>
            </div>
          )}
        </div>
      </div>

      {/* Left: Form */}
      <div className="relative z-0 flex min-h-dvh w-full items-center justify-center px-4 py-10 sm:px-10 lg:justify-start lg:pl-24 lg:pr-14">
        <div className="w-full max-w-md lg:translate-x-60">
          <div className="rounded-2xl border border-slate-200/70 bg-white/85 p-6 shadow-[0_18px_60px_rgba(0,0,0,0.10)] backdrop-blur sm:p-8">
            <div className="mb-2">
              <h1 className="text-2xl font-semibold tracking-tight mb-8">
                {t('sys.login.sign_in_to_device', 'Sign in to device')}
              </h1>
            </div>

            <form className="space-y-6" onSubmit={handleLogin}>
              <div>
                <label
                  htmlFor="username"
                  className="mb-2 block text-sm font-medium text-slate-800"
                >
                  {t('sys.login.username', 'Username')}
                </label>
                <div className="relative">
                  <div className="pointer-events-none absolute inset-y-0 left-0 flex items-center pl-3 text-slate-400">
                    <svg
                      xmlns="http://www.w3.org/2000/svg"
                      className="h-5 w-5"
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="2"
                      strokeLinecap="round"
                      strokeLinejoin="round"
                    >
                      <path d="M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2" />
                      <circle cx="12" cy="7" r="4" />
                    </svg>
                  </div>
                  <input
                    id="username"
                    value={values.username}
                    onChange={event => {
                      const { value } = event.target;
                      setValues(prev => ({ ...prev, username: value }));
                      if (errors.username) {
                        setErrors(prev => ({ ...prev, username: undefined }));
                      }
                    }}
                    placeholder={t(
                      'sys.login.username_placeholder',
                      'Enter username (e.g. admin)'
                    )}
                    disabled={isLoading}
                    autoComplete="username"
                    className="h-12 w-full rounded-xl border border-slate-200 bg-white px-3 py-3 pl-10 text-sm text-slate-900 outline-none transition focus:border-primary/40 focus:bg-transparent focus:ring-2 focus:ring-primary/20 disabled:opacity-60"
                  />
                </div>
                {errors.username && (
                  <p className="text-xs text-destructive mt-2 font-medium">
                    {errors.username}
                  </p>
                )}
              </div>

              <div>
                <label
                  htmlFor="password"
                  className="mb-2 block text-sm font-medium text-slate-800"
                >
                  {t('sys.login.password', 'Password')}
                </label>
                <div className="relative">
                  <div className="pointer-events-none absolute inset-y-0 left-0 flex items-center pl-3 text-slate-400">
                    <svg
                      xmlns="http://www.w3.org/2000/svg"
                      className="h-5 w-5"
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="2"
                      strokeLinecap="round"
                      strokeLinejoin="round"
                    >
                      <rect width="18" height="11" x="3" y="11" rx="2" ry="2" />
                      <path d="M7 11V7a5 5 0 0 1 10 0v4" />
                    </svg>
                  </div>
                  <input
                    id="password"
                    type={showPassword ? 'text' : 'password'}
                    value={values.password}
                    onChange={event => {
                      const { value } = event.target;
                      setValues(prev => ({ ...prev, password: value }));
                      if (errors.password) {
                        setErrors(prev => ({ ...prev, password: undefined }));
                      }
                    }}
                    placeholder={t(
                      'sys.login.password_placeholder',
                      '••••••••'
                    )}
                    disabled={isLoading}
                    autoComplete="current-password"
                    className="h-12 w-full rounded-xl border border-slate-200 bg-white px-3 py-3 pl-10 pr-11 text-sm text-slate-900 outline-none transition focus:border-primary/40 focus:bg-transparent focus:ring-2 focus:ring-primary/20 disabled:opacity-60"
                  />
                  <button
                    type="button"
                    className="absolute right-3 top-1/2 inline-flex h-9 w-9 -translate-y-1/2 items-center justify-center rounded-lg text-slate-500 hover:text-slate-900"
                    onClick={() => setShowPassword(prev => !prev)}
                    aria-label={
                      showPassword
                        ? t('sys.login.hide_password', 'Hide password')
                        : t('sys.login.show_password', 'Show password')
                    }
                  >
                    {showPassword ? (
                      <EyeOff className="h-5 w-5" />
                    ) : (
                      <Eye className="h-5 w-5" />
                    )}
                  </button>
                </div>
                {errors.password && (
                  <p className="text-xs text-destructive mt-2 font-medium">
                    {errors.password}
                  </p>
                )}
              </div>

              <div className="pt-2">
                <button
                  type="submit"
                  className="group flex h-12 w-full items-center justify-center rounded-xl bg-[#0D0D0D] px-4 text-sm font-semibold text-white shadow-md transition hover:bg-black focus:outline-none focus:ring-2 focus:ring-primary/25 focus:ring-offset-2 disabled:opacity-60"
                  disabled={isLoading}
                >
                  {isLoading ? (
                    <>
                      <Loader2 className="h-4 w-4 mr-2 animate-spin" />
                      {t('common.loading')}
                    </>
                  ) : (
                    <>
                      {t('sys.login.login_now', 'Sign in')}
                      <ArrowRight className="h-4 w-4 ml-1 group-hover:translate-x-1 transition-transform" />
                    </>
                  )}
                </button>
              </div>
            </form>
          </div>
        </div>
      </div>
    </div>
  );
}
