import type { MockMethod } from 'vite-plugin-mock';

// 模拟用户数据
const mockAdminPassword = 'mock-admin-password';
const mockUserPassword = 'mock-user-password';

const mockUsers = [
  {
    username: 'admin',
    password: mockAdminPassword,
  },
  {
    username: 'user',
    password: mockUserPassword,
  },
];

export default [
  {
    url: '/api/v1/login',
    method: 'post',
    timeout: 1000, // 模拟网络延迟 1 秒
    response: ({
      body,
    }: {
      body: { username?: string; password?: string };
    }) => {
      const { username, password } = body || {};

      // 参数验证
      if (!username || !password) {
        return {
          success: false,
          code: 400,
          message: '用户名和密码不能为空',
          data: null,
        };
      }

      // 验证用户名和密码
      const user = mockUsers.find(
        u => u.username === username && u.password === password
      );

      if (!user) {
        return {
          success: false,
          code: 401,
          message: '用户名或密码错误',
          data: null,
        };
      }

      // 生成 token（实际项目中应该由后端生成）
      const credentials = `${username}:${password}`;
      const token = `Basic ${btoa(credentials)}`;

      return {
        success: true,
        code: 200,
        message: '登录成功',
        data: {
          token,
          username: user.username,
        },
      };
    },
  },
  {
    url: '/api/v1/logout',
    method: 'post',
    timeout: 500,
    response: () => ({
      success: true,
      code: 200,
      message: '登出成功',
      data: null,
    }),
  },
] as MockMethod[];
