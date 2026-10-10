async (page) => {
  await page.setViewportSize({width:1600,height:1000});
  const button = page.locator('.tw-node[data-definition="core.spawn"]').getByRole('button',{name:'选择对象模板',exact:true});
  await button.click();
  const panel = page.getByRole('dialog',{name:'对象模板选择器',exact:true});
  await panel.waitFor({state:'visible'});
  await page.screenshot({path:'output/playwright/r03-picker-first.png'});
  return { panel:await panel.boundingBox(),node:await page.locator('.tw-node[data-definition="core.spawn"]').boundingBox(),sourceTabs:await panel.getByRole('tab').allTextContents() };
}
