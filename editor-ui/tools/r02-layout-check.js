async (page) => {
  const checks = [];
  const assert = (ok, text) => { if (!ok) throw new Error(text); checks.push(text); };
  await page.getByRole('button', { name: '打开工程', exact: true }).click();
  await page.getByRole('button', { name: '打开示例工程', exact: true }).click();
  await page.getByRole('button', { name: '物体模块', exact: true }).click();
  const objectBounds = await page.getByRole('region', { name: '物体编辑器', exact: true }).boundingBox();
  assert(objectBounds.width > 1450, '物编工作区填满桌面宽度');
  const health = page.getByRole('spinbutton', { name: '最大生命值', exact: true });
  await health.fill('515'); await health.press('Tab');
  assert(await health.inputValue() === '515', '物编生命值可提交');
  await page.getByRole('button', { name: '撤销', exact: true }).click();
  assert(await health.inputValue() === '420', '统一撤销还原物编修改');
  await page.screenshot({ path: 'output/playwright/workbench-r02-objects.png' });
  const sizes = [];
  for (const module of ['触发器', '物体', '变量', '声音', '资源', '物体管理', '战役', 'AI', '世界']) {
    await page.getByRole('button', { name: `${module}模块`, exact: true }).click();
    const dimensions = await page.evaluate(() => ({ width: innerWidth, scroll: document.documentElement.scrollWidth, bodyWidth: document.querySelector('.wb-workspace-slot').getBoundingClientRect().width, childWidth: document.querySelector('.wb-workspace-slot').firstElementChild.getBoundingClientRect().width }));
    assert(dimensions.scroll <= dimensions.width && dimensions.childWidth >= dimensions.bodyWidth - 2, `${module}模块填满工作区且页面无横向溢出`);
    sizes.push({ module, ...dimensions });
    if (['触发器', '声音', '战役', 'AI'].includes(module)) await page.screenshot({ path: `output/playwright/workbench-r02-${{ '触发器': 'triggers', '声音': 'sound', '战役': 'campaign', 'AI': 'ai' }[module]}.png` });
  }
  await page.getByRole('button', { name: '保存工程', exact: true }).click();
  const stored = await page.evaluate(() => JSON.parse(localStorage.getItem('forge-workbench-r02')));
  assert(stored.objects.length === 19 && stored.variables.length === 6 && stored.sounds.length === 2, '九模块共用工程保存包含物编、变量与声音');
  await page.getByRole('button', { name: '声音模块', exact: true }).click();
  return { passed: checks.length, checks, sizes, soundSnapshot: (await page.locator('.wb-workspace-slot').ariaSnapshot()).slice(0, 9500) };
}
