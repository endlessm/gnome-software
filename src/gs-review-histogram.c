/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2016 Canonical Ltd.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <math.h>

#include "gs-review-histogram.h"
#include "gs-review-bar.h"
#include "gs-star-image.h"

typedef struct
{
	GtkWidget	*bar1;
	GtkWidget	*bar2;
	GtkWidget	*bar3;
	GtkWidget	*bar4;
	GtkWidget	*bar5;
	GtkWidget	*label_value;
	GtkWidget	*label_total;
	GtkWidget	*star_value_1;
	GtkWidget	*star_value_2;
	GtkWidget	*star_value_3;
	GtkWidget	*star_value_4;
	GtkWidget	*star_value_5;
} GsReviewHistogramPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsReviewHistogram, gs_review_histogram, GTK_TYPE_BIN)

void
gs_review_histogram_set_ratings (GsReviewHistogram *histogram,
				 GArray *review_ratings)
{
	GsReviewHistogramPrivate *priv = gs_review_histogram_get_instance_private (histogram);
	g_autofree gchar *text = NULL;
	gdouble fraction[6] = { 0.0f };
	guint32 max = 0;
	guint32 total = 0;
	guint32 star_count = 0;

	g_return_if_fail (GS_IS_REVIEW_HISTOGRAM (histogram));

	/* sanity check */
	if (review_ratings->len != 6) {
		g_warning ("ratings data incorrect expected 012345");
		return;
	}

	/* idx 0 is '0 stars' which we don't support in the UI */
	for (guint i = 1; i < review_ratings->len; i++) {
		guint32 c = g_array_index (review_ratings, guint32, i);
		max = MAX (c, max);
		star_count += i * c;
	}
	for (guint i = 1; i < review_ratings->len; i++) {
		guint32 c = g_array_index (review_ratings, guint32, i);
		fraction[i] = max > 0 ? (gdouble) c / (gdouble) max : 0.f;
		total += c;
	}

	gs_review_bar_set_fraction (GS_REVIEW_BAR (priv->bar5), fraction[5]);
	gs_review_bar_set_fraction (GS_REVIEW_BAR (priv->bar4), fraction[4]);
	gs_review_bar_set_fraction (GS_REVIEW_BAR (priv->bar3), fraction[3]);
	gs_review_bar_set_fraction (GS_REVIEW_BAR (priv->bar2), fraction[2]);
	gs_review_bar_set_fraction (GS_REVIEW_BAR (priv->bar1), fraction[1]);

	text = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u review total", "%u reviews total", total), total);
	gtk_label_set_text (GTK_LABEL (priv->label_total), text);

	g_clear_pointer (&text, g_free);

	/* Round explicitly, to avoid rounding inside the printf() call and to use
	   the same value also for the stars fraction. */
	fraction[0] = total > 0 ? round (((gdouble) star_count / (gdouble) total) * 10.0) / 10.0 : 0.0;
	text = g_strdup_printf ("%.01f", fraction[0]);
	gtk_label_set_text (GTK_LABEL (priv->label_value), text);

	gs_star_image_set_fraction (GS_STAR_IMAGE (priv->star_value_1), CLAMP (fraction[0], 0.0, 1.0));
	gs_star_image_set_fraction (GS_STAR_IMAGE (priv->star_value_2), CLAMP (fraction[0], 1.0, 2.0) - 1.0);
	gs_star_image_set_fraction (GS_STAR_IMAGE (priv->star_value_3), CLAMP (fraction[0], 2.0, 3.0) - 2.0);
	gs_star_image_set_fraction (GS_STAR_IMAGE (priv->star_value_4), CLAMP (fraction[0], 3.0, 4.0) - 3.0);
	gs_star_image_set_fraction (GS_STAR_IMAGE (priv->star_value_5), CLAMP (fraction[0], 4.0, 5.0) - 4.0);
}

static void
gs_review_histogram_init (GsReviewHistogram *histogram)
{
	gtk_widget_init_template (GTK_WIDGET (histogram));
}

static void
gs_review_histogram_class_init (GsReviewHistogramClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-review-histogram.ui");

	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, bar5);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, bar4);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, bar3);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, bar2);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, bar1);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_value);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_total);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, star_value_1);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, star_value_2);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, star_value_3);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, star_value_4);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, star_value_5);
}

GtkWidget *
gs_review_histogram_new (void)
{
	GsReviewHistogram *histogram;
	histogram = g_object_new (GS_TYPE_REVIEW_HISTOGRAM, NULL);
	return GTK_WIDGET (histogram);
}
