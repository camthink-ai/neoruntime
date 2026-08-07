import { Button, buttonVariants } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover';
import { cn } from '@/lib/utils';
import { add, format } from 'date-fns';
import { type Locale, enUS } from 'date-fns/locale';
import { zhCN } from 'date-fns/locale/zh-CN';
import {
  Calendar as CalendarIcon,
  ChevronLeft,
  ChevronRight,
  Clock,
  X,
} from 'lucide-react';
import * as React from 'react';
import { useImperativeHandle, useRef } from 'react';
import { useTranslation } from 'react-i18next';

import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import {
  DayPicker,
  type DayPickerProps,
  type DropdownProps,
} from 'react-day-picker';

// ---------- utils start ----------
/**
 * regular expression to check for valid hour format (01-23)
 */
function isValidHour(value: string) {
  return /^(0[0-9]|1[0-9]|2[0-3])$/.test(value);
}

/**
 * regular expression to check for valid 12 hour format (01-12)
 */
function isValid12Hour(value: string) {
  return /^(0[1-9]|1[0-2])$/.test(value);
}

/**
 * regular expression to check for valid minute format (00-59)
 */
function isValidMinuteOrSecond(value: string) {
  return /^[0-5][0-9]$/.test(value);
}

type GetValidNumberConfig = { max: number; min?: number; loop?: boolean };

function getValidNumber(
  value: string,
  { max, min = 0, loop = false }: GetValidNumberConfig
) {
  let numericValue = parseInt(value, 10);

  if (!Number.isNaN(numericValue)) {
    if (!loop) {
      if (numericValue > max) numericValue = max;
      if (numericValue < min) numericValue = min;
    } else {
      if (numericValue > max) numericValue = min;
      if (numericValue < min) numericValue = max;
    }
    return numericValue.toString().padStart(2, '0');
  }

  return '00';
}

function getValidHour(value: string) {
  if (isValidHour(value)) return value;
  return getValidNumber(value, { max: 23 });
}

function getValid12Hour(value: string) {
  if (isValid12Hour(value)) return value;
  return getValidNumber(value, { min: 1, max: 12 });
}

function getValidMinuteOrSecond(value: string) {
  if (isValidMinuteOrSecond(value)) return value;
  return getValidNumber(value, { max: 59 });
}

type GetValidArrowNumberConfig = {
  min: number;
  max: number;
  step: number;
};

function getValidArrowNumber(
  value: string,
  { min, max, step }: GetValidArrowNumberConfig
) {
  let numericValue = parseInt(value, 10);
  if (!Number.isNaN(numericValue)) {
    numericValue += step;
    return getValidNumber(String(numericValue), { min, max, loop: true });
  }
  return '00';
}

function getValidArrowHour(value: string, step: number) {
  return getValidArrowNumber(value, { min: 0, max: 23, step });
}

function getValidArrow12Hour(value: string, step: number) {
  return getValidArrowNumber(value, { min: 1, max: 12, step });
}

function getValidArrowMinuteOrSecond(value: string, step: number) {
  return getValidArrowNumber(value, { min: 0, max: 59, step });
}

function setMinutes(date: Date, value: string) {
  const minutes = getValidMinuteOrSecond(value);
  date.setMinutes(parseInt(minutes, 10));
  return date;
}

function setSeconds(date: Date, value: string) {
  const seconds = getValidMinuteOrSecond(value);
  date.setSeconds(parseInt(seconds, 10));
  return date;
}

function setHours(date: Date, value: string) {
  const hours = getValidHour(value);
  date.setHours(parseInt(hours, 10));
  return date;
}

function set12Hours(date: Date, value: string, period: Period) {
  const hours = parseInt(getValid12Hour(value), 10);
  const convertedHours = convert12HourTo24Hour(hours, period);
  date.setHours(convertedHours);
  return date;
}

type TimePickerType = 'minutes' | 'seconds' | 'hours' | '12hours';
type Period = 'AM' | 'PM';

function setDateByType(
  date: Date,
  value: string,
  type: TimePickerType,
  period?: Period
) {
  switch (type) {
    case 'minutes':
      return setMinutes(date, value);
    case 'seconds':
      return setSeconds(date, value);
    case 'hours':
      return setHours(date, value);
    case '12hours': {
      if (!period) return date;
      return set12Hours(date, value, period);
    }
    default:
      return date;
  }
}

function getDateByType(date: Date | null, type: TimePickerType) {
  if (!date) return '00';
  switch (type) {
    case 'minutes':
      return getValidMinuteOrSecond(String(date.getMinutes()));
    case 'seconds':
      return getValidMinuteOrSecond(String(date.getSeconds()));
    case 'hours':
      return getValidHour(String(date.getHours()));
    case '12hours':
      return getValid12Hour(String(display12HourValue(date.getHours())));
    default:
      return '00';
  }
}

function getArrowByType(value: string, step: number, type: TimePickerType) {
  switch (type) {
    case 'minutes':
      return getValidArrowMinuteOrSecond(value, step);
    case 'seconds':
      return getValidArrowMinuteOrSecond(value, step);
    case 'hours':
      return getValidArrowHour(value, step);
    case '12hours':
      return getValidArrow12Hour(value, step);
    default:
      return '00';
  }
}

/**
 * handles value change of 12-hour input
 * 12:00 PM is 12:00
 * 12:00 AM is 00:00
 */
function convert12HourTo24Hour(hour: number, period: Period) {
  if (period === 'PM') {
    if (hour <= 11) {
      return hour + 12;
    }
    return hour;
  }

  if (period === 'AM') {
    if (hour === 12) return 0;
    return hour;
  }
  return hour;
}

/**
 * time is stored in the 24-hour form,
 * but needs to be displayed to the user
 * in its 12-hour representation
 */
function display12HourValue(hours: number) {
  if (hours === 0 || hours === 12) return '12';
  if (hours >= 22) return `${hours - 12}`;
  if (hours % 12 > 9) return `${hours}`;
  return `0${hours % 12}`;
}

// ---------- utils end ----------

/** Map app i18n language to date-fns locale */
const DATE_FNS_LOCALES: Record<string, Locale> = {
  zh: zhCN,
  en: enUS,
  'zh-CN': zhCN,
  'en-US': enUS,
};

function useDatePickerLocale() {
  const { i18n } = useTranslation();
  // Try exact match first, then base language
  const currentLang = i18n.language;
  const baseLang = currentLang.split('-')[0];
  return DATE_FNS_LOCALES[currentLang] ?? DATE_FNS_LOCALES[baseLang] ?? enUS;
}

function CalendarDropdown({ value, onChange, options }: DropdownProps) {
  const handleValueChange = React.useCallback(
    (nextValue: string) => {
      if (!onChange) {
        return;
      }

      const changeEvent = {
        target: { value: nextValue },
      } as React.ChangeEvent<HTMLSelectElement>;

      onChange(changeEvent);
    },
    [onChange]
  );

  return (
    <Select value={value?.toString()} onValueChange={handleValueChange}>
      <SelectTrigger
        className="focus:bg-accent focus:text-accent-foreground h-8 w-fit min-w-0 gap-1.5 border-none px-1.5 shadow-none"
        onPointerDownCapture={e => e.stopPropagation()}
        onClick={e => e.stopPropagation()}
      >
        <SelectValue />
      </SelectTrigger>
      <SelectContent portalled={false} position="popper">
        {options?.map(option => (
          <SelectItem key={option.value} value={option.value.toString()}>
            {option.label}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  );
}

function Calendar({
  className,
  classNames,
  showOutsideDays = true,
  yearRange = 50,
  ...props
}: DayPickerProps & { yearRange?: number }) {
  const datePickerLocale = useDatePickerLocale();
  const today = React.useMemo(() => new Date(), []);
  const startMonth = React.useMemo(
    () => new Date(today.getFullYear() - yearRange, 0, 1),
    [today, yearRange]
  );
  const endMonth = React.useMemo(
    () => new Date(today.getFullYear() + yearRange, 11, 31),
    [today, yearRange]
  );
  const disableLeftNavigation = () => {
    if (props.month) {
      return (
        props.month.getMonth() === startMonth.getMonth()
        && props.month.getFullYear() === startMonth.getFullYear()
      );
    }
    return false;
  };
  const disableRightNavigation = () => {
    if (props.month) {
      return (
        props.month.getMonth() === endMonth.getMonth()
        && props.month.getFullYear() === endMonth.getFullYear()
      );
    }
    return false;
  };

  const components = React.useMemo(
    () => ({
      Chevron: ({ ...chevronProps }) => (chevronProps.orientation === 'left' ? (
          <ChevronLeft className="h-4 w-4" />
        ) : (
          <ChevronRight className="h-4 w-4" />
        )),
      Dropdown: CalendarDropdown,
    }),
    []
  );

  return (
    <DayPicker
      showOutsideDays={showOutsideDays}
      captionLayout="dropdown"
      startMonth={startMonth}
      endMonth={endMonth}
      locale={props.locale ?? datePickerLocale}
      className={cn('p-3 w-80', className)}
      classNames={{
        months:
          'flex flex-col sm:flex-row space-y-4  sm:space-y-0 justify-center',
        month: 'flex flex-col items-center space-y-4',
        month_caption: 'relative flex items-center justify-center px-6 pt-1',
        dropdowns: 'flex items-center justify-center gap-2',
        dropdown_root: 'w-fit',
        caption_label: 'text-sm font-medium',
        nav: 'space-x-1 flex items-center ',
        button_previous: cn(
          buttonVariants({ variant: 'outline' }),
          'h-7 w-7 bg-transparent p-0 opacity-50 hover:opacity-100 absolute left-5 top-5',
          disableLeftNavigation() && 'pointer-events-none'
        ),
        button_next: cn(
          buttonVariants({ variant: 'outline' }),
          'h-7 w-7 bg-transparent p-0 opacity-50 hover:opacity-100 absolute right-5 top-5',
          disableRightNavigation() && 'pointer-events-none'
        ),
        month_grid: 'mx-auto w-full border-collapse space-y-1',
        weekdays: 'flex justify-center',
        weekday:
          'text-muted-foreground rounded-md w-8 font-normal text-[0.8rem]',
        week: 'flex w-full mt-2 justify-center',
        day: 'h-8 w-8 text-center text-sm p-0 relative [&:has([aria-selected])]:bg-accent [&:has([aria-selected].day-range-end)]:rounded-r-md [&:has([aria-selected].day-outside)]:bg-accent/50 focus-within:relative focus-within:z-20',
        day_button: cn(
          buttonVariants({ variant: 'ghost' }),
          'h-8 w-8 p-0 font-normal aria-selected:opacity-100'
        ),
        range_end: 'day-range-end',
        selected:
          'bg-primary text-primary-foreground hover:bg-primary hover:text-primary-foreground focus:bg-primary focus:text-primary-foreground rounded-md',
        today: 'bg-accent text-accent-foreground rounded-md',
        outside:
          'day-outside text-muted-foreground aria-selected:bg-accent/50 aria-selected:text-muted-foreground',
        disabled: 'text-muted-foreground opacity-50',
        range_middle:
          'aria-selected:bg-accent aria-selected:text-accent-foreground',
        hidden: 'invisible',
        ...classNames,
      }}
      components={components}
      {...props}
    />
  );
}
Calendar.displayName = 'Calendar';

interface PeriodSelectorProps {
  period: Period;
  setPeriod?: (m: Period) => void;
  date?: Date | null;
  onDateChange?: (date: Date | undefined) => void;
  onRightFocus?: () => void;
  onLeftFocus?: () => void;
}

const TimePeriodSelect = React.forwardRef<
  HTMLButtonElement,
  PeriodSelectorProps
>(
  (
    { period, setPeriod, date, onDateChange, onLeftFocus, onRightFocus },
    ref
  ) => {
    const handleKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>) => {
      if (e.key === 'ArrowRight') onRightFocus?.();
      if (e.key === 'ArrowLeft') onLeftFocus?.();
    };

    const handleValueChange = (value: Period) => {
      setPeriod?.(value);

      /**
       * trigger an update whenever the user switches between AM and PM;
       * otherwise user must manually change the hour each time
       */
      if (date) {
        const tempDate = new Date(date);
        const hours = display12HourValue(date.getHours());
        onDateChange?.(
          setDateByType(
            tempDate,
            hours.toString(),
            '12hours',
            period === 'AM' ? 'PM' : 'AM'
          )
        );
      }
    };

    return (
      <div className="flex h-10 items-center">
        <Select
          defaultValue={period}
          onValueChange={(value: Period) => handleValueChange(value)}
        >
          <SelectTrigger
            ref={ref}
            className="focus:bg-accent focus:text-accent-foreground w-[65px]"
            onKeyDown={handleKeyDown}
          >
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="AM">AM</SelectItem>
            <SelectItem value="PM">PM</SelectItem>
          </SelectContent>
        </Select>
      </div>
    );
  }
);

TimePeriodSelect.displayName = 'TimePeriodSelect';

interface TimePickerInputProps extends React.InputHTMLAttributes<HTMLInputElement> {
  picker: TimePickerType;
  date?: Date | null;
  onDateChange?: (date: Date | undefined) => void;
  period?: Period;
  onRightFocus?: () => void;
  onLeftFocus?: () => void;
  /**
   * Whether to show the browser clear ("×") affordance and clear the picker value
   * when the input becomes empty.
   */
  clearable?: boolean;
}

const TimePickerInput = React.forwardRef<
  HTMLInputElement,
  TimePickerInputProps
>(
  (
    {
      className,
      type = 'tel',
      value,
      id,
      name,
      date = new Date(new Date().setHours(0, 0, 0, 0)),
      onDateChange,
      onChange,
      onKeyDown,
      picker,
      period,
      onLeftFocus,
      onRightFocus,
      clearable = true,
      ...props
    },
    ref
  ) => {
    const [flag, setFlag] = React.useState<boolean>(false);
    const [prevIntKey, setPrevIntKey] = React.useState<string>('0');

    /**
     * allow the user to enter the second digit within 2 seconds
     * otherwise start again with entering first digit
     */
    React.useEffect(() => {
      if (flag) {
        const timer = setTimeout(() => {
          setFlag(false);
        }, 2000);

        return () => clearTimeout(timer);
      }
    }, [flag]);

    const calculatedValue = React.useMemo(
      () => getDateByType(date, picker),
      [date, picker]
    );

    const calculateNewValue = (key: string) => {
      /*
       * If picker is '12hours' and the first digit is 0, then the second digit is automatically set to 1.
       * The second entered digit will break the condition and the value will be set to 10-12.
       */
      if (picker === '12hours') {
        if (flag && calculatedValue.slice(1, 2) === '1' && prevIntKey === '0') return `0${key}`;
      }

      return !flag ? `0${key}` : calculatedValue.slice(1, 2) + key;
    };

    const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
      if (e.key === 'Tab') return;
      e.preventDefault();
      if (e.key === 'ArrowRight') onRightFocus?.();
      if (e.key === 'ArrowLeft') onLeftFocus?.();
      if (['ArrowUp', 'ArrowDown'].includes(e.key)) {
        const step = e.key === 'ArrowUp' ? 1 : -1;
        const newValue = getArrowByType(calculatedValue, step, picker);
        if (flag) setFlag(false);
        const tempDate = date ? new Date(date) : new Date();
        onDateChange?.(setDateByType(tempDate, newValue, picker, period));
      }
      if (e.key >= '0' && e.key <= '9') {
        if (picker === '12hours') setPrevIntKey(e.key);

        const newValue = calculateNewValue(e.key);
        if (flag) onRightFocus?.();
        setFlag(prev => !prev);
        const tempDate = date ? new Date(date) : new Date();
        onDateChange?.(setDateByType(tempDate, newValue, picker, period));
      }
    };

    return (
      <Input
        ref={ref}
        id={id || picker}
        name={name || picker}
        className={cn(
          'focus:bg-accent focus:text-accent-foreground w-[48px] text-center font-mono text-base tabular-nums caret-transparent [&::-webkit-inner-spin-button]:appearance-none',
          !clearable
            && '[&::-webkit-search-cancel-button]:hidden [&::-ms-clear]:hidden [&::-webkit-clear-button]:hidden',
          className
        )}
        value={value || calculatedValue}
        onChange={e => {
          const nextValue = e.currentTarget.value;

          if (nextValue === '') {
            if (!clearable) return;
            onDateChange?.(undefined);
            return;
          }

          e.preventDefault();
          onChange?.(e);
        }}
        type={type}
        inputMode="decimal"
        onKeyDown={e => {
          onKeyDown?.(e);
          handleKeyDown(e);
        }}
        {...props}
      />
    );
  }
);

TimePickerInput.displayName = 'TimePickerInput';

interface TimePickerProps {
  date?: Date | null;
  onChange?: (date: Date | undefined) => void;
  hourCycle?: 12 | 24;
  /**
   * Determines the smallest unit that is displayed in the datetime picker.
   * Default is 'second'.
   * */
  granularity?: Granularity;
  clearable?: boolean;
}

interface TimePickerRef {
  minuteRef: HTMLInputElement | null;
  hourRef: HTMLInputElement | null;
  secondRef: HTMLInputElement | null;
}

const TimePicker = React.forwardRef<TimePickerRef, TimePickerProps>(
  (
    {
      date: timePickerDate,
      onChange,
      hourCycle = 24,
      granularity = 'second',
      clearable = true,
    },
    ref
  ) => {
    const minuteRef = React.useRef<HTMLInputElement>(null);
    const hourRef = React.useRef<HTMLInputElement>(null);
    const secondRef = React.useRef<HTMLInputElement>(null);
    const periodRef = React.useRef<HTMLButtonElement>(null);
    const [period, setPeriod] = React.useState<Period>(
      timePickerDate && timePickerDate.getHours() >= 12 ? 'PM' : 'AM'
    );

    useImperativeHandle(
      ref,
      () => ({
        minuteRef: minuteRef.current,
        hourRef: hourRef.current,
        secondRef: secondRef.current,
        periodRef: periodRef.current,
      }),
      [minuteRef, hourRef, secondRef]
    );
    return (
      <div className="flex items-center justify-center gap-2">
        <label htmlFor="datetime-picker-hour-input" className="cursor-pointer">
          <Clock className="h-4 w-4" />
        </label>
        <TimePickerInput
          picker={hourCycle === 24 ? 'hours' : '12hours'}
          date={timePickerDate}
          id="datetime-picker-hour-input"
          onDateChange={onChange}
          ref={hourRef}
          period={period}
          onRightFocus={() => minuteRef?.current?.focus()}
          clearable={clearable}
        />
        {(granularity === 'minute' || granularity === 'second') && (
          <>
            :
            <TimePickerInput
              picker="minutes"
              date={timePickerDate}
              onDateChange={onChange}
              ref={minuteRef}
              onLeftFocus={() => hourRef?.current?.focus()}
              onRightFocus={() => secondRef?.current?.focus()}
              clearable={clearable}
            />
          </>
        )}
        {granularity === 'second' && (
          <>
            :
            <TimePickerInput
              picker="seconds"
              date={timePickerDate}
              onDateChange={onChange}
              ref={secondRef}
              onLeftFocus={() => minuteRef?.current?.focus()}
              onRightFocus={() => periodRef?.current?.focus()}
              clearable={clearable}
            />
          </>
        )}
        {hourCycle === 12 && (
          <div className="grid gap-1 text-center">
            <TimePeriodSelect
              period={period}
              setPeriod={setPeriod}
              date={timePickerDate}
              onDateChange={date => {
                onChange?.(date);
                if (date && date?.getHours() >= 12) {
                  setPeriod('PM');
                } else {
                  setPeriod('AM');
                }
              }}
              ref={periodRef}
              onLeftFocus={() => secondRef?.current?.focus()}
            />
          </div>
        )}
      </div>
    );
  }
);
TimePicker.displayName = 'TimePicker';

type Granularity = 'day' | 'hour' | 'minute' | 'second';

type DateTimePickerProps = {
  value?: Date;
  onChange?: (date: Date | undefined) => void;
  onMonthChange?: (date: Date | undefined) => void;
  disabled?: boolean;
  /**
   * Whether the browser clear ("×") affordance is enabled for the internal
   * time inputs, and whether clearing should also clear the selected value.
   * @defaultValue true
   */
  clearable?: boolean;
  /** showing `AM/PM` or not. */
  hourCycle?: 12 | 24;
  placeholder?: string;
  /**
   * The year range will be: `This year + yearRange` and `this year - yearRange`.
   * Default is 50.
   * For example:
   * This year is 2024, The year dropdown will be 1974 to 2024 which is generated by `2024 - 50 = 1974` and `2024 + 50 = 2074`.
   * */
  yearRange?: number;
  /**
   * The format is derived from the `date-fns` documentation.
   * @reference https://date-fns.org/v3.6.0/docs/format
   * */
  displayFormat?: { hour24?: string; hour12?: string };
  t?: any;
  /**
   * The granularity prop allows you to control the smallest unit that is displayed by DateTimePicker.
   * By default, the value is `second` which shows all time inputs.
   * */
  granularity?: Granularity;
  className?: string;
  /**
   * Show the default month and time when popup the calendar. Default is the current Date().
   * */
  defaultPopupValue?: Date;
} & Pick<
  DayPickerProps,
  'locale' | 'weekStartsOn' | 'showWeekNumber' | 'showOutsideDays'
>;

type DateTimePickerRef = {
  value?: Date;
} & Omit<HTMLButtonElement, 'value'>;

const DateTimePicker = React.forwardRef<
  Partial<DateTimePickerRef>,
  DateTimePickerProps
>(
  (
    {
      locale: localeProp,
      defaultPopupValue = new Date(new Date().setHours(0, 0, 0, 0)),
      value,
      onChange,
      onMonthChange,
      hourCycle = 24,
      yearRange = 50,
      disabled = false,
      clearable = true,
      displayFormat,
      granularity = 'second',
      placeholder,
      className,
      ...props
    },
    ref
  ) => {
    const { t } = useTranslation();
    const datePickerLocale = useDatePickerLocale();
    const locale = localeProp ?? datePickerLocale;
    const finalPlaceholder = placeholder || t('common.pick_date', '选择日期');
    const [month, setMonth] = React.useState<Date>(value ?? defaultPopupValue);
    const buttonRef = useRef<HTMLButtonElement>(null);
    const [displayDate, setDisplayDate] = React.useState<Date | undefined>(
      value ?? undefined
    );
    onMonthChange ||= onChange;

    const valueTimestamp = React.useMemo(
      () => (value instanceof Date ? value.getTime() : undefined),
      [value]
    );

    /**
     * Sync internal state from controlled value by timestamp instead of
     * Date object identity, so callers passing `new Date(...)` on each
     * render do not continuously reset the picker state.
     */
    React.useEffect(() => {
      setDisplayDate(prev => {
        if (valueTimestamp === undefined) {
          return undefined;
        }
        if (prev?.getTime() === valueTimestamp) {
          return prev;
        }
        return new Date(valueTimestamp);
      });

      if (valueTimestamp !== undefined) {
        setMonth(prev => {
          if (prev?.getTime() === valueTimestamp) {
            return prev;
          }
          return new Date(valueTimestamp);
        });
      }
    }, [valueTimestamp]);

    /**
     * carry over the current time when a user clicks a new day
     * instead of resetting to 00:00
     */
    const handleMonthChange = React.useCallback(
      (newDay: Date | undefined) => {
        if (!newDay) {
          return;
        }
        if (!defaultPopupValue) {
          newDay.setHours(
            month?.getHours() ?? 0,
            month?.getMinutes() ?? 0,
            month?.getSeconds() ?? 0
          );
          onMonthChange?.(newDay);
          setMonth(newDay);
          return;
        }
        const diff = newDay.getTime() - defaultPopupValue.getTime();
        const diffInDays = diff / (1000 * 60 * 60 * 24);
        const newDateFull = add(defaultPopupValue, {
          days: Math.ceil(diffInDays),
        });
        newDateFull.setHours(
          month?.getHours() ?? 0,
          month?.getMinutes() ?? 0,
          month?.getSeconds() ?? 0
        );
        onMonthChange?.(newDateFull);
        setMonth(newDateFull);
      },
      [defaultPopupValue, month, onMonthChange]
    );

    const onSelect = React.useCallback(
      (newDay?: Date) => {
        if (!newDay) {
          return;
        }
        onChange?.(newDay);
        setMonth(newDay);
        setDisplayDate(newDay);
      },
      [onChange]
    );

    const shouldShowClearIcon = clearable && !!displayDate;

    const handleClear = React.useCallback(() => {
      if (disabled) return;
      onChange?.(undefined);
      onMonthChange?.(undefined);
      setDisplayDate(undefined);
      // Reset to default month for calendar UI consistency
      setMonth(new Date(defaultPopupValue));
    }, [disabled, onChange, onMonthChange, defaultPopupValue]);

    useImperativeHandle(
      ref,
      () => ({
        ...buttonRef.current,
        value: displayDate,
      }),
      [displayDate]
    );

    const initHourFormat = {
      hour24:
        displayFormat?.hour24
        ?? `PPP HH:mm${!granularity || granularity === 'second' ? ':ss' : ''}`,
      hour12:
        displayFormat?.hour12
        ?? `PP hh:mm${!granularity || granularity === 'second' ? ':ss' : ''} b`,
    };

    let loc = locale;
    const { options, localize, formatLong } = locale;
    if (options && localize && formatLong) {
      loc = {
        ...locale,
        options,
        localize,
        formatLong,
      };
    }

    return (
      <Popover modal={false}>
        <PopoverTrigger asChild disabled={disabled}>
          <Button
            variant="outline"
            className={cn(
              'flex items-center gap-2 justify-start text-left font-normal',
              className,
              clearable && 'w-[260px]',
              !displayDate
                && 'text-muted-foreground bg-transparent! hover:bg-transparent! dark:bg-transparent! dark:hover:bg-transparent!'
            )}
            ref={buttonRef}
          >
            <CalendarIcon className="h-4 w-4" />
            <span className="flex-1">
              {displayDate ? (
                format(
                  displayDate,
                  hourCycle === 24
                    ? initHourFormat.hour24
                    : initHourFormat.hour12,
                  {
                    locale: loc as Locale,
                  }
                )
              ) : (
                <span>{finalPlaceholder}</span>
              )}
            </span>
            {clearable ? (
              shouldShowClearIcon ? (
                <span
                  role="button"
                  tabIndex={0}
                  aria-label={t('common.clear', '清空')}
                  className="ml-auto inline-flex size-5 cursor-pointer shrink-0 items-center justify-center rounded-full bg-muted text-muted-foreground hover:bg-muted/80"
                  onClick={e => {
                    e.preventDefault();
                    e.stopPropagation();
                    handleClear();
                  }}
                  onKeyDown={e => {
                    if (e.key !== 'Enter' && e.key !== ' ') return;
                    e.preventDefault();
                    e.stopPropagation();
                    handleClear();
                  }}
                >
                  <X className="h-1.5 w-1.5" />
                </span>
              ) : (
                <span className="ml-auto size-5 shrink-0" aria-hidden />
              )
            ) : null}
          </Button>
        </PopoverTrigger>
        <PopoverContent className="w-auto p-0">
          <Calendar
            mode="single"
            selected={displayDate}
            month={month}
            onSelect={newDate => {
              if (newDate) {
                newDate.setHours(
                  month?.getHours() ?? 0,
                  month?.getMinutes() ?? 0,
                  month?.getSeconds() ?? 0
                );
                onSelect(newDate);
              }
            }}
            onMonthChange={handleMonthChange}
            yearRange={yearRange}
            locale={locale}
            {...props}
          />
          {granularity !== 'day' && (
            <div className="border-border border-t p-3">
              <TimePicker
                onChange={newValue => {
                  onChange?.(newValue);
                  setDisplayDate(newValue);
                  if (newValue) {
                    setMonth(newValue);
                  } else {
                    // Keep the calendar/date selection cleared, but reset time inputs
                    // to a deterministic default.
                    setMonth(defaultPopupValue);
                  }
                }}
                date={month}
                hourCycle={hourCycle}
                granularity={granularity}
                clearable={clearable}
              />
            </div>
          )}
        </PopoverContent>
      </Popover>
    );
  }
);

DateTimePicker.displayName = 'DateTimePicker';

export { DateTimePicker, TimePickerInput, TimePicker };
export type { TimePickerType, DateTimePickerProps, DateTimePickerRef };
