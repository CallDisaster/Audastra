import { Printer, ArrowLeft, ArrowRight } from "lucide-react";

export function DesignSheet({ close }: { close: () => void }) {
  return (
    <div className="design-sheet">
      <header>
        <button onClick={close}>
          <ArrowLeft size={16} />
          返回编辑器
        </button>
        <span>FORGE / DESIGN SYSTEM</span>
        <button onClick={() => window.print()}>
          <Printer size={16} />
          打印图纸
        </button>
      </header>
      <main>
        <div className="sheet-heading">
          <div>
            <span className="eyebrow">EDITOR WORKBENCH / R01</span>
            <h1>通用编辑器 · 界面图纸</h1>
          </div>
          <span>
            2026.09.22
            <br />
            桌面工作台 / 自适应
          </span>
        </div>
        <section>
          <div className="sheet-section-title">
            <b>01</b>
            <h2>工作台布局</h2>
            <span>1440 × 900 基准</span>
          </div>
          <div className="layout-drawing">
            <div className="dw-header">
              01 应用栏 <small>工程 / 命令 / 保存 / 导出</small>
              <b>52</b>
            </div>
            <div className="dw-toolbar">
              02 文档与工具栏 <small>文档 / 撤销 / 视图 / 检查</small>
              <b>40 + 42</b>
            </div>
            <div className="dw-body">
              <div className="dw-rail">
                模块
                <br />
                56
              </div>
              <div className="dw-tree">
                03 工程树
                <small>
                  蓝图
                  <br />
                  脚本
                  <br />
                  数据
                  <br />
                  资源
                </small>
                <strong>224</strong>
              </div>
              <div className="dw-canvas">
                <span>04 主编辑区</span>
                <div className="dw-flow">
                  <i>事件</i>
                  <ArrowRight />
                  <i>条件</i>
                  <ArrowRight />
                  <i>动作</i>
                </div>
                <small>蓝图 / 脚本 / 数据 / 资源 / 扩展</small>
                <em>自适应宽度 · 保持画布可操作</em>
              </div>
              <div className="dw-inspector">
                05 属性
                <small>
                  节点定义
                  <br />
                  参数
                  <br />
                  端口
                  <br />
                  来源
                </small>
                <strong>272</strong>
              </div>
            </div>
            <div className="dw-bottom">
              06 诊断抽屉 <small>错误 / 警告 / 定位</small>
            </div>
            <div className="dw-status">
              07 状态栏 <small>本地保存 / 文档 / 连接状态</small>
              <b>25</b>
            </div>
          </div>
        </section>
        <div className="sheet-columns">
          <section>
            <div className="sheet-section-title">
              <b>02</b>
              <h2>视觉令牌</h2>
            </div>
            <div className="swatches">
              {[
                ["#151719", "画布"],
                ["#1b1e20", "面板"],
                ["#303538", "分隔"],
                ["#e5e9e7", "文本"],
                ["#8ad6b0", "选中"],
              ].map(([c, l]) => (
                <div key={c}>
                  <i style={{ background: c }} />
                  <b>{l}</b>
                  <code>{c}</code>
                </div>
              ))}
            </div>
            <div className="type-spec">
              <span>Aa 字</span>
              <div>
                Segoe UI Variable / 微软雅黑
                <small>
                  界面 12–14 px · 文档标题 20 px
                  <br />
                  代码 Cascadia Code / Consolas
                </small>
              </div>
            </div>
            <p>间距 4 / 8 / 12 / 16 / 24 圆角 6 / 8 分隔线 1 px</p>
          </section>
          <section>
            <div className="sheet-section-title">
              <b>03</b>
              <h2>编辑状态</h2>
            </div>
            <div className="states">
              <button>默认</button>
              <button className="hovered">悬停</button>
              <button className="primary">选中</button>
              <button disabled>不可用</button>
            </div>
            <dl>
              <dt>已知定义</dt>
              <dd>创建 · 编辑 · 复制 · 连线</dd>
              <dt>缺少定义</dt>
              <dd>保留内容 · 定位缺失项 · 导入定义</dd>
              <dt>输入错误</dt>
              <dd>字段内标记 · 修正后提交</dd>
              <dt>本地保存失败</dt>
              <dd>保留当前内容 · 导出工程文件</dd>
            </dl>
          </section>
        </div>
        <section>
          <div className="sheet-section-title">
            <b>04</b>
            <h2>模块与数据关系</h2>
          </div>
          <div className="architecture">
            <div>蓝图 / 脚本 / 数据 / 资源</div>
            <ArrowRight />
            <div>命令 · 撤销 · 选择</div>
            <ArrowRight />
            <div>通用工程模型</div>
            <ArrowRight />
            <div>
              定义注册表
              <br />
              格式适配器
            </div>
          </div>
          <div className="spec-caption">
            函数能力由定义与适配器提供。工程名称、地图路径和函数编号不参与前端权限判断。
          </div>
        </section>
        <div className="sheet-columns">
          <section>
            <div className="sheet-section-title">
              <b>05</b>
              <h2>蓝图交互</h2>
            </div>
            <dl>
              <dt>拖动节点</dt>
              <dd>8px 网格吸附 · 连线跟随</dd>
              <dt>点击端口</dt>
              <dd>选择另一端 · 类型校验</dd>
              <dt>空白拖动 / 滚轮</dt>
              <dd>平移 / 缩放</dd>
              <dt>选择连线 + Delete</dt>
              <dd>删除连线 · 可撤销</dd>
              <dt>Ctrl + Z / Y / S / K</dt>
              <dd>撤销 / 重做 / 保存 / 命令</dd>
            </dl>
          </section>
          <section>
            <div className="sheet-section-title">
              <b>06</b>
              <h2>窗口适配</h2>
            </div>
            <dl>
              <dt>≥ 1440 px</dt>
              <dd>工程树 + 编辑区 + 属性</dd>
              <dt>1024–1439 px</dt>
              <dd>紧凑工程树与属性</dd>
              <dt>720–1023 px</dt>
              <dd>折叠工程树 · 按需打开属性</dd>
              <dt>&lt; 720 px</dt>
              <dd>单编辑区 · 属性覆盖层</dd>
              <dt>减少动画</dt>
              <dd>关闭过渡与入场动画</dd>
            </dl>
          </section>
        </div>
        <footer>
          FORGE STUDIO <span>前端交互实现 · 原生格式适配器待接入</span>R01 / 01
        </footer>
      </main>
    </div>
  );
}
