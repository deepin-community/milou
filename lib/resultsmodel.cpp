/*
 * This file is part of the KDE Milou Project
 * SPDX-FileCopyrightText: 2019 Kai Uwe Broulik <kde@broulik.de>
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 */

#include "resultsmodel.h"

#include "runnerresultsmodel.h"

#include <KRunner/RunnerManager>
#include <QIdentityProxyModel>

#include <KDescendantsProxyModel>
#include <KModelIndexProxyMapper>

#include <KRunner/AbstractRunner>
#include <cmath>

using namespace Milou;

/**
 * Sorts the matches and categories by their type and relevance
 *
 * A category gets type and relevance of the highest
 * scoring match within.
 */
class SortProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    SortProxyModel(QObject *parent)
        : QSortFilterProxyModel(parent)
    {
        setDynamicSortFilter(true);
        sort(0, Qt::DescendingOrder);
    }
    ~SortProxyModel() override = default;

    void setQueryString(const QString &queryString)
    {
        const QStringList words = queryString.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (m_words != words) {
            m_words = words;
            invalidate();
        }
    }

    bool categoryHasMatchWithAllWords(const QModelIndex &categoryIdx) const
    {
        for (int i = 0; i < sourceModel()->rowCount(categoryIdx); ++i) {
            const QModelIndex idx = sourceModel()->index(i, 0, categoryIdx);
            const QString display = idx.data(Qt::DisplayRole).toString();

            bool containsAllWords = true;
            for (const QString &word : m_words) {
                if (!display.contains(word, Qt::CaseInsensitive)) {
                    containsAllWords = false;
                }
            }

            if (containsAllWords) {
                return true;
            }
        }

        return false;
    }

protected:
    bool lessThan(const QModelIndex &sourceA, const QModelIndex &sourceB) const override
    {
        // prefer categories that have a match containing the query string in the display role
        if (!sourceA.parent().isValid() && !sourceB.parent().isValid()) {
            const bool hasMatchWithAllWordsA = categoryHasMatchWithAllWords(sourceA);
            const bool hasMatchWithAllWordsB = categoryHasMatchWithAllWords(sourceB);

            if (hasMatchWithAllWordsA != hasMatchWithAllWordsB) {
                return !hasMatchWithAllWordsA && hasMatchWithAllWordsB;
            }
        }

        const int typeA = sourceA.data(ResultsModel::TypeRole).toInt();
        const int typeB = sourceB.data(ResultsModel::TypeRole).toInt();

        if (typeA != typeB) {
            return typeA < typeB;
        }

        const qreal relevanceA = sourceA.data(ResultsModel::RelevanceRole).toReal();
        const qreal relevanceB = sourceB.data(ResultsModel::RelevanceRole).toReal();

        if (!qFuzzyCompare(relevanceA, relevanceB)) {
            return relevanceA < relevanceB;
        }

        return QSortFilterProxyModel::lessThan(sourceA, sourceB);
    }

private:
    QStringList m_words;
};

/**
 * Distributes the number of matches shown per category
 *
 * Each category may occupy a maximum of 1/(n+1) of the given @c limit,
 * this means the further down you get, the less matches there are.
 * There is at least one match shown per category.
 *
 * This model assumes the results to already be sorted
 * descending by their relevance/score.
 */
class CategoryDistributionProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    CategoryDistributionProxyModel(QObject *parent)
        : QSortFilterProxyModel(parent)
    {
    }
    ~CategoryDistributionProxyModel() override = default;

    void setSourceModel(QAbstractItemModel *sourceModel) override
    {
        if (this->sourceModel()) {
            disconnect(this->sourceModel(), nullptr, this, nullptr);
        }

        QSortFilterProxyModel::setSourceModel(sourceModel);

        if (sourceModel) {
            connect(sourceModel, &QAbstractItemModel::rowsInserted, this, &CategoryDistributionProxyModel::invalidateFilter);
            connect(sourceModel, &QAbstractItemModel::rowsMoved, this, &CategoryDistributionProxyModel::invalidateFilter);
            connect(sourceModel, &QAbstractItemModel::rowsRemoved, this, &CategoryDistributionProxyModel::invalidateFilter);
        }
    }

    int limit() const
    {
        return m_limit;
    }

    void setLimit(int limit)
    {
        if (m_limit == limit) {
            return;
        }
        m_limit = limit;
        invalidateFilter();
        Q_EMIT limitChanged();
    }

Q_SIGNALS:
    void limitChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (m_limit <= 0) {
            return true;
        }

        if (!sourceParent.isValid()) {
            return true;
        }

        const int categoryCount = sourceModel()->rowCount();

        int maxItemsInCategory = m_limit;

        if (categoryCount > 1) {
            int itemsBefore = 0;
            for (int i = 0; i <= sourceParent.row(); ++i) {
                const int itemsInCategory = sourceModel()->rowCount(sourceModel()->index(i, 0));

                // Take into account that every category gets at least one item shown
                const int availableSpace = m_limit - itemsBefore - std::ceil(m_limit / qreal(categoryCount));

                // The further down the category is the less relevant it is and the less space it my occupy
                // First category gets max half the total limit, second category a third, etc
                maxItemsInCategory = std::min(availableSpace, int(std::ceil(m_limit / qreal(i + 2))));

                // At least show one item per category
                maxItemsInCategory = std::max(1, maxItemsInCategory);

                itemsBefore += std::min(itemsInCategory, maxItemsInCategory);
            }
        }

        if (sourceRow >= maxItemsInCategory) {
            return false;
        }

        return true;
    }

private:
    // if you change this, update the default in resetLimit()
    int m_limit = 0;
};

/**
 * This model hides the root items of data originally in a tree structure
 *
 * KDescendantsProxyModel collapses the items but keeps all items in tact.
 * The root items of the RunnerMatchesModel represent the individual cateories
 * which we don't want in the resulting flat list.
 * This model maps the items back to the given @c treeModel and filters
 * out any item with an invalid parent, i.e. "on the root level"
 */
class HideRootLevelProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    HideRootLevelProxyModel(QObject *parent)
        : QSortFilterProxyModel(parent)
    {
    }
    ~HideRootLevelProxyModel() override = default;

    QAbstractItemModel *treeModel() const
    {
        return m_treeModel;
    }
    void setTreeModel(QAbstractItemModel *treeModel)
    {
        m_treeModel = treeModel;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        KModelIndexProxyMapper mapper(sourceModel(), m_treeModel);
        const QModelIndex treeIdx = mapper.mapLeftToRight(sourceModel()->index(sourceRow, 0, sourceParent));
        return treeIdx.parent().isValid();
    }

private:
    QAbstractItemModel *m_treeModel = nullptr;
};

/**
 * Populates the IsDuplicateRole of an item
 *
 * The IsDuplicateRole returns true for each item if there is two or more
 * elements in the model with the same DisplayRole as the item.
 */
class DuplicateDetectorProxyModel : public QIdentityProxyModel
{
    Q_OBJECT

public:
    DuplicateDetectorProxyModel(QObject *parent)
        : QIdentityProxyModel(parent)
    {
    }
    ~DuplicateDetectorProxyModel() override = default;

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role != ResultsModel::DuplicateRole) {
            return QIdentityProxyModel::data(index, role);
        }

        int duplicatesCount = 0;
        const QString display = index.data(Qt::DisplayRole).toString();

        for (int i = 0; i < sourceModel()->rowCount(); ++i) {
            if (sourceModel()->index(i, 0).data(Qt::DisplayRole) == display) {
                ++duplicatesCount;

                if (duplicatesCount == 2) {
                    return true;
                }
            }
        }

        return false;
    }
};

class Q_DECL_HIDDEN ResultsModel::Private
{
public:
    Private(ResultsModel *q);

    ResultsModel *q;

    QPointer<Plasma::AbstractRunner> runner = nullptr;

    RunnerResultsModel *resultsModel;
    SortProxyModel *sortModel;
    CategoryDistributionProxyModel *distributionModel;
    KDescendantsProxyModel *flattenModel;
    HideRootLevelProxyModel *hideRootModel;
    DuplicateDetectorProxyModel *duplicateDetectorModel;
};

ResultsModel::Private::Private(ResultsModel *q)
    : q(q)
    , resultsModel(new RunnerResultsModel(q))
    , sortModel(new SortProxyModel(q))
    , distributionModel(new CategoryDistributionProxyModel(q))
    , flattenModel(new KDescendantsProxyModel(q))
    , hideRootModel(new HideRootLevelProxyModel(q))
    , duplicateDetectorModel(new DuplicateDetectorProxyModel(q))
{
}

ResultsModel::ResultsModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , d(new Private(this))
{
    connect(d->resultsModel, &RunnerResultsModel::queryStringChanged, this, &ResultsModel::queryStringChanged);
    connect(d->resultsModel, &RunnerResultsModel::queryingChanged, this, &ResultsModel::queryingChanged);
    connect(d->resultsModel, &RunnerResultsModel::queryStringChangeRequested, this, &ResultsModel::queryStringChangeRequested);

    connect(d->resultsModel, &RunnerResultsModel::queryStringChanged, d->sortModel, &SortProxyModel::setQueryString);

    connect(d->distributionModel, &CategoryDistributionProxyModel::limitChanged, this, &ResultsModel::limitChanged);

    // The data flows as follows:
    // - RunnerResultsModel
    //   - SortProxyModel
    //     - CategoryDistributionProxyModel
    //       - KDescendantsProxyModel
    //         - HideRootLevelProxyModel
    //           - DuplicateDetectorProxyModel

    d->sortModel->setSourceModel(d->resultsModel);

    d->distributionModel->setSourceModel(d->sortModel);

    d->flattenModel->setSourceModel(d->distributionModel);

    d->hideRootModel->setSourceModel(d->flattenModel);
    d->hideRootModel->setTreeModel(d->resultsModel);

    d->duplicateDetectorModel->setSourceModel(d->hideRootModel);

    setSourceModel(d->duplicateDetectorModel);
}

ResultsModel::~ResultsModel() = default;

QString ResultsModel::queryString() const
{
    return d->resultsModel->queryString();
}

void ResultsModel::setQueryString(const QString &queryString)
{
    d->resultsModel->setQueryString(queryString, runner());
}

int ResultsModel::limit() const
{
    return d->distributionModel->limit();
}

void ResultsModel::setLimit(int limit)
{
    d->distributionModel->setLimit(limit);
}

void ResultsModel::resetLimit()
{
    setLimit(0);
}

bool ResultsModel::querying() const
{
    return d->resultsModel->querying();
}

QString ResultsModel::runner() const
{
    return d->runner ? d->runner->id() : QString();
}

void ResultsModel::setRunner(const QString &runnerId)
{
    if (runnerId == runner()) {
        return;
    }
    if (runnerId.isEmpty()) {
        d->runner = nullptr;
    } else {
        d->runner = runnerManager()->runner(runnerId);
    }
    Q_EMIT runnerChanged();
}

QString ResultsModel::runnerName() const
{
    return d->runner ? d->runner->name() : QString();
}

QIcon ResultsModel::runnerIcon() const
{
    return d->runner ? d->runner->icon() : QIcon();
}

QHash<int, QByteArray> ResultsModel::roleNames() const
{
    auto names = QAbstractItemModel::roleNames();
    names[IdRole] = QByteArrayLiteral("matchId"); // "id" is QML-reserved
    names[EnabledRole] = QByteArrayLiteral("enabled");
    names[TypeRole] = QByteArrayLiteral("type");
    names[RelevanceRole] = QByteArrayLiteral("relevance");
    names[CategoryRole] = QByteArrayLiteral("category");
    names[SubtextRole] = QByteArrayLiteral("subtext");
    names[DuplicateRole] = QByteArrayLiteral("isDuplicate");
    names[ActionsRole] = QByteArrayLiteral("actions");
    names[MultiLineRole] = QByteArrayLiteral("multiLine");
    return names;
}

void ResultsModel::clear()
{
    d->resultsModel->clear();
}

bool ResultsModel::run(const QModelIndex &idx)
{
    KModelIndexProxyMapper mapper(this, d->resultsModel);
    const QModelIndex resultsIdx = mapper.mapLeftToRight(idx);
    if (!resultsIdx.isValid()) {
        return false;
    }
    return d->resultsModel->run(resultsIdx);
}

bool ResultsModel::runAction(const QModelIndex &idx, int actionNumber)
{
    KModelIndexProxyMapper mapper(this, d->resultsModel);
    const QModelIndex resultsIdx = mapper.mapLeftToRight(idx);
    if (!resultsIdx.isValid()) {
        return false;
    }
    return d->resultsModel->runAction(resultsIdx, actionNumber);
}

QMimeData *ResultsModel::getMimeData(const QModelIndex &idx) const
{
    KModelIndexProxyMapper mapper(this, d->resultsModel);
    const QModelIndex resultsIdx = mapper.mapLeftToRight(idx);
    if (!resultsIdx.isValid()) {
        return nullptr;
    }
    return d->resultsModel->mimeData({resultsIdx});
}

Plasma::RunnerManager *Milou::ResultsModel::runnerManager() const
{
    return d->resultsModel->runnerManager();
}

#include "resultsmodel.moc"
