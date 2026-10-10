async (page) => {
  const checks = [];
  const assert = (ok, message) => { if (!ok) throw new Error(message); checks.push(message); };
  await page.getByRole('button', { name: '打开工程', exact: true }).click();
  await page.getByRole('button', { name: '打开示例工程', exact: true }).click();
  await page.getByRole('button', { name: '变量模块', exact: true }).click();
  await page.getByRole('textbox', { name: '变量名称 wave', exact: true }).fill('currentWave');
  await page.getByRole('textbox', { name: '变量名称 wave', exact: true }).press('Tab');
  assert(await page.locator('.ow-modal').getByText('wave < maxWaves', { exact: true }).isVisible(), '变量改名显示未解析表达式的待核对引用');
  await page.locator('.ow-modal').getByRole('button', { name: '取消', exact: true }).click();
  await page.getByRole('textbox', { name: '初始值 spawnCount', exact: true }).fill('7');
  await page.getByRole('textbox', { name: '初始值 spawnCount', exact: true }).press('Tab');
  await page.getByRole('button', { name: '触发器模块', exact: true }).click();
  await page.getByRole('button', { name: '代码', exact: true }).click();
  assert(/udg_spawnCount\s*=\s*7/.test(await page.getByRole('textbox', { name: '触发器代码', exact: true }).inputValue()), '全局变量初值联动触发器生成预览');
  await page.getByRole('button', { name: '物体模块', exact: true }).click();
  await page.getByRole('button', { name: 'A 技能 3', exact: true }).click();
  await page.getByRole('button', { name: '等级矩阵', exact: true }).click();
  const cooldown = page.getByRole('spinbutton', { name: '冷却时间 等级 1', exact: true });
  await cooldown.fill('12'); await cooldown.press('Tab');
  await page.getByRole('button', { name: '复制末级', exact: true }).click();
  assert(await page.getByRole('spinbutton', { name: '冷却时间 等级 4', exact: true }).inputValue() === '8', '物编等级矩阵复制末级保留原有等级');
  await page.getByRole('button', { name: '恢复默认', exact: true }).click();
  assert(await cooldown.inputValue() === '10' && await page.getByRole('spinbutton', { name: '冷却时间 等级 4', exact: true }).count() === 0, '等级恢复默认还原数值与等级数量');
  await page.screenshot({ path: 'output/playwright/workbench-r02-levels.png' });
  for (const size of [[1440, 900], [1280, 800], [1024, 768]]) {
    await page.setViewportSize({ width: size[0], height: size[1] });
    for (const module of ['触发器', '物体', '声音', 'AI']) {
      await page.getByRole('button', { name: `${module}模块`, exact: true }).click();
      const layout = await page.evaluate(() => ({ width: innerWidth, scroll: document.documentElement.scrollWidth, work: document.querySelector('.wb-workspace-slot').getBoundingClientRect().width }));
      assert(layout.scroll <= layout.width && layout.work > 800, `${module} ${size[0]}×${size[1]} 桌面布局有效`);
    }
  }
  await page.setViewportSize({ width: 1600, height: 1000 });
  await page.getByRole('button', { name: '设置', exact: true }).click();
  await page.getByRole('combobox', { name: '界面主题', exact: true }).selectOption('paper');
  await page.getByRole('button', { name: '关闭窗口', exact: true }).click();
  assert(await page.locator('.theme-paper').count() === 1, '浅色主题切换成功');
  await page.getByRole('button', { name: '设置', exact: true }).click();
  await page.getByRole('combobox', { name: '界面主题', exact: true }).selectOption('graphite');
  await page.getByRole('button', { name: '关闭窗口', exact: true }).click();
  await page.getByRole('button', { name: '打开工程', exact: true }).click();
  await page.getByRole('button', { name: '打开示例工程', exact: true }).click();
  await page.getByRole('button', { name: '保存工程', exact: true }).click();
  return { passed: checks.length, checks };
}
