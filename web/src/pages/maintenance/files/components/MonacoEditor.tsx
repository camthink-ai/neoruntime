import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import Editor, { type OnMount } from '@monaco-editor/react';
import type * as Monaco from 'monaco-editor';
import Loading from '@/components/loading';

interface MonacoEditorProps {
  value: string;
  language?: string;
  readOnly?: boolean;
  onChange?: (value: string) => void;
  filename?: string;
}

// 根据文件名推断语言
const getLanguageFromFilename = (filename: string): string => {
  const ext = filename.split('.').pop()?.toLowerCase() || '';

  const langMap: Record<string, string> = {
    js: 'javascript',
    jsx: 'javascript',
    ts: 'typescript',
    tsx: 'typescript',
    py: 'python',
    java: 'java',
    c: 'c',
    cpp: 'cpp',
    h: 'c',
    hpp: 'cpp',
    go: 'go',
    rs: 'rust',
    php: 'php',
    rb: 'ruby',
    sh: 'shell',
    bash: 'shell',
    css: 'css',
    scss: 'scss',
    html: 'html',
    xml: 'xml',
    json: 'json',
    yaml: 'yaml',
    yml: 'yaml',
    toml: 'toml',
    ini: 'ini',
    conf: 'plaintext',
    txt: 'plaintext',
    md: 'markdown',
    log: 'plaintext',
    csv: 'plaintext',
  };

  return langMap[ext] || 'plaintext';
};

export default function MonacoEditor({
  value,
  language,
  readOnly = false,
  onChange,
  filename,
}: MonacoEditorProps) {
  const { t } = useTranslation();
  const editorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);
  const detectedLanguage =    language || (filename ? getLanguageFromFilename(filename) : 'plaintext');

  const handleEditorDidMount: OnMount = (
    editorInstance: Monaco.editor.IStandaloneCodeEditor
  ) => {
    editorRef.current = editorInstance;
  };

  useEffect(() => {
    // 当组件挂载时，聚焦编辑器
    if (editorRef.current && !readOnly) {
      editorRef.current.focus();
    }
  }, [readOnly]);

  return (
    <Editor
      height="100%"
      language={detectedLanguage}
      value={value}
      onChange={(val: string | undefined) => onChange?.(val || '')}
      onMount={handleEditorDidMount}
      theme="vs-dark"
      options={{
        readOnly,
        minimap: { enabled: true },
        fontSize: 14,
        lineNumbers: 'on',
        scrollBeyondLastLine: false,
        automaticLayout: true,
        wordWrap: 'on',
        scrollbar: {
          vertical: 'auto',
          horizontal: 'auto',
        },
        cursorStyle: readOnly ? 'line-thin' : 'line',
        renderWhitespace: 'selection',
        folding: true,
        links: true,
        mouseWheelZoom: true,
        // 当前行高亮
        renderLineHighlight: 'all',
      }}
      loading={(
        <Loading
          fullHeight={false}
          className="h-full bg-[#1e1e1e]"
          size="sm"
          placeholder={t(
            'sys.file_management.loading_editor',
            'Loading editor...'
          )}
        />
      )}
    />
  );
}
