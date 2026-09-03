# OpenRead C++ 绑定 / Qt 集成

## 两种集成方式

### 方式一：直接链接静态库（推荐）

最简单，直接链接 `libopenread.a`，使用 C++ 便捷头文件：

```cpp
#include <openread/wrapper.hpp>

openread::Engine engine;
engine.loadSourcesFromFile("sources.json");

auto books = engine.search("斗破苍穹");
for (const auto& book : books) {
    qDebug() << book.name.c_str() << "by" << book.author.c_str();
}
```

**CMakeLists.txt**:
```cmake
# 方式一：直接编译源码
set(OPENREAD_DIR ${CMAKE_SOURCE_DIR}/third_party/OpenRead)
add_subdirectory(${OPENREAD_DIR})

target_link_libraries(myapp PRIVATE openread)
```

### 方式二：链接共享库（适合闭源分发）

```cmake
# 方式二：链接预编译共享库
find_library(OPENREAD_LIBRARY openread PATHS /usr/local/lib)
target_link_libraries(myapp PRIVATE ${OPENREAD_LIBRARY})
target_include_directories(myapp PRIVATE /usr/local/include)
```

## Qt 示例：简单阅读器

```cpp
// main.cpp
#include <QApplication>
#include <QMainWindow>
#include <QListWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <openread/wrapper.hpp>

class ReaderWindow : public QMainWindow {
    Q_OBJECT
public:
    ReaderWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        // 初始化引擎
        engine_.loadSourcesFromFile("sources.json");

        // 设置 HTTP 回调（用 Qt 网络栈）
        engine_.setHttpCallback([this](
            const std::string& url, const std::string& method,
            const std::string& headers, const std::string& body
        ) -> std::string {
            // 同步 HTTP 请求（实际项目应异步）
            QNetworkRequest request(QUrl(QString::fromStdString(url)));
            QNetworkReply* reply = network_.get(request);
            // ... 等待完成 ...
            return reply->readAll().toStdString();
        });

        // UI 布局
        auto* splitter = new QSplitter(Qt::Horizontal, this);

        searchEdit_ = new QLineEdit(this);
        searchBtn_ = new QPushButton("搜索", this);
        bookList_ = new QListWidget(this);
        chapterList_ = new QListWidget(this);
        contentEdit_ = new QTextEdit(this);
        contentEdit_->setReadOnly(true);

        // ... 布局代码省略 ...

        connect(searchBtn_, &QPushButton::clicked, [this]() {
            bookList_->clear();
            auto books = engine_.search(searchEdit_->text().toStdString());
            for (const auto& book : books) {
                bookList_->addItem(
                    QString("%1 - %2")
                        .arg(book.name.c_str())
                        .arg(book.author.c_str())
                );
                books_.push_back(book);
            }
        });

        connect(bookList_, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
            int row = bookList_->row(item);
            chapterList_->clear();
            auto chapters = engine_.getCatalog(books_[row].bookUrl);
            for (const auto& ch : chapters) {
                chapterList_->addItem(ch.title.c_str());
                chapters_.push_back(ch);
            }
        });

        connect(chapterList_, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
            int row = chapterList_->row(item);
            std::string content = engine_.getContent(chapters_[row].url);
            contentEdit_->setText(QString::fromStdString(content));
        });

        setCentralWidget(splitter);
        resize(1200, 800);
    }

private:
    openread::Engine engine_;
    QNetworkAccessManager network_;
    QLineEdit* searchEdit_;
    QPushButton* searchBtn_;
    QListWidget* bookList_;
    QListWidget* chapterList_;
    QTextEdit* contentEdit_;
    std::vector<openread::Book> books_;
    std::vector<openread::Chapter> chapters_;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    ReaderWindow window;
    window.show();
    return app.exec();
}
```

## C API 直接调用（适用于纯 C / JNI / Swift 等）

```c
#include <openread/bridge.h>

OpenReadEngine engine = openread_engine_create();
openread_engine_load_sources_from_file(engine, "sources.json");

char* result = openread_engine_search(engine, "斗破苍穹");
printf("Search result: %s\n", result);
openread_free_string(result);

openread_engine_destroy(engine);
```
